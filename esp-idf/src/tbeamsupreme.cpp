/**
 * tbeamsupreme.cpp — LilyGo T-Beam S3 Supreme board support, end to end.
 *
 * Single owner of all Supreme hardware bring-up. See tbeamsupreme.h for the
 * API contract. Layout:
 *
 *   1. AXP2101 PMU: rails for the SX1262, the display/sensor I2C bus, the
 *      microSD and (in a build with gps staged) the GNSS receiver. Always
 *      compiled. Driven from onStart() before spangapInit().
 *   2. CS park for the two devices on the shared peripheral SPI bus (microSD
 *      and the QMI8658 IMU).
 *
 * Rail map, from LilyGo's t_beam_supreme_hw.md PowerManage table (and matching
 * Meshtastic's tbeam-s3-core): DC1 = the ESP32-S3 itself, ALDO1 = display +
 * BME280 + magnetometer, ALDO2 = the sensor rail the 17/18 I2C bus needs,
 * ALDO3 = radio, ALDO4 = GNSS, BLDO1 = microSD, BLDO2 = the external pin
 * header, DC3/4/5 = the external M.2 socket. Nothing on those rails answers
 * until the PMU switches them on, so this runs before any driver touches its
 * peripheral. The rails that only feed connectors stay off. Register writes go
 * straight at the AXP2101 (enable ctrl
 * 0x90, voltage regs 0x92..0x96, 100 mV steps from 0.5 V) — a full PMU driver
 * (charging, fuel gauge, IRQ) is not this straddle's problem until something
 * needs it.
 */
#include "tbeamsupreme.h"

#include "i2c_helper.h"     /* SPANGAP_I2C_PULLUP (shared bus wiring policy) */
#include "log.h"            /* warn (IMU absent) */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* AXP2101 registers. Enable bits in 0x90: ALDO1..4 = bits 0..3, BLDO1 = bit 4;
 * the voltage register of each follows the same order from 0x92. */
#define AXP2101_LDO_EN0      0x90
#define AXP2101_ALDO1_V      0x92
#define AXP2101_ALDO2_V      0x93
#define AXP2101_ALDO3_V      0x94
#define AXP2101_ALDO4_V      0x95
#define AXP2101_BLDO1_V      0x96
#define AXP2101_EN_ALDO1     0x01
#define AXP2101_EN_ALDO2     0x02
#define AXP2101_EN_ALDO3     0x04
#define AXP2101_EN_ALDO4     0x08
#define AXP2101_EN_BLDO1     0x10
#define AXP2101_MV(mv)       (uint8_t)(((mv) - 500) / 100)

static i2c_master_dev_handle_t s_pmu = nullptr;

static bool pmuWrite(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pmu, buf, 2, pdMS_TO_TICKS(100)) == ESP_OK;
}

static bool pmuRead(uint8_t reg, uint8_t* val)
{
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1,
                                       pdMS_TO_TICKS(100)) == ESP_OK;
}

/* Enable/disable a set of the LDO bits, keeping every other bit of 0x90 —
 * read-modify-write on purpose, because a blind write would also decide the
 * fate of rails this board keeps off (and of anything a future driver turned
 * on). DC1, the rail the ESP32-S3 itself runs from, lives in another register
 * and cannot be reached from here at all. */
static void pmuRails(uint8_t bits, bool on)
{
    uint8_t en = 0;
    if (!pmuRead(AXP2101_LDO_EN0, &en)) return;
    pmuWrite(AXP2101_LDO_EN0, on ? (uint8_t)(en | bits) : (uint8_t)(en & ~bits));
}

/* =========================================================================
 * 1. PMU rails
 * ========================================================================= */

static void tbeamSupremePmuInit(void)
{
    i2c_master_bus_config_t bus = {};
    /* Controller 0, named rather than auto-allocated: the PCF8563 shares these
     * wires, and spangap-rtc adopts this bus by port number
     * (CONFIG_RTC_I2C_PORT). tinylcd auto-allocates and gets the other one. */
    bus.i2c_port          = 0;
    bus.sda_io_num        = (gpio_num_t)BOARD_PMU_SDA_PIN;
    bus.scl_io_num        = (gpio_num_t)BOARD_PMU_SCL_PIN;
    bus.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
    i2c_master_bus_handle_t h = nullptr;
    if (i2c_new_master_bus(&bus, &h) != ESP_OK) return;
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = BOARD_PMU_I2C_ADDR;
    dev.scl_speed_hz    = 100000;
    if (i2c_master_bus_add_device(h, &dev, &s_pmu) != ESP_OK) return;

    /* Cold boot: power the display/sensor and microSD rails DOWN first. The
     * PMU keeps its rail state across an ESP32 reset, so a card left mid-
     * transaction by the previous firmware comes back still holding the SPI
     * bus, and a sensor mid-transaction still holding I2C — neither of which a
     * bus reset from this side clears. LilyGo's own board support does the
     * same cycle for the same reason. A wake from sleep skips it: the rails
     * were ours all along, and the blink costs the SD a remount. */
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        pmuRails(AXP2101_EN_ALDO1 | AXP2101_EN_ALDO2 | AXP2101_EN_BLDO1, false);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    /* 3.3 V on the radio, display, sensor-bus and SD rails, then enable them
     * in one read-modify-write. ALDO2 is not optional: the 17/18 I2C bus needs
     * it up before anything on it is addressed, or a read there fails or hangs
     * (LilyGo's hw doc says so explicitly). ALDO4, the GNSS rail, joins them
     * only in a build that carries the gps straddle — a receiver nothing reads
     * is pure drain on a battery board. */
    uint8_t rails = AXP2101_EN_ALDO1 | AXP2101_EN_ALDO2 |
                    AXP2101_EN_ALDO3 | AXP2101_EN_BLDO1;
    pmuWrite(AXP2101_ALDO1_V, AXP2101_MV(3300));
    pmuWrite(AXP2101_ALDO2_V, AXP2101_MV(3300));
    pmuWrite(AXP2101_ALDO3_V, AXP2101_MV(3300));
    pmuWrite(AXP2101_BLDO1_V, AXP2101_MV(3300));
#if CONFIG_STRADDLE_GPS
    pmuWrite(AXP2101_ALDO4_V, AXP2101_MV(3300));
    rails |= AXP2101_EN_ALDO4;
#endif
    pmuRails(rails, true);
    vTaskDelay(pdMS_TO_TICKS(20));   /* rail settle before anyone probes */
}

/* =========================================================================
 * 2. Peripheral-bus CS park
 * ========================================================================= */

static void tbeamSupremeCsPark(void)
{
    /* The microSD and the QMI8658 IMU share one SPI bus (the radio has its own,
     * per LilyGo's pin map), so park BOTH CS lines HIGH before the SD probe in
     * spangapInit() runs: nothing in this build drives the IMU, and a floating
     * CS is all it takes for it to answer onto MISO in the middle of the card's
     * initialisation. The SD's CS pin exists only when the card is configured
     * in. */
    uint64_t mask = 1ULL << BOARD_IMU_CS_PIN;
#if defined(CONFIG_SPANGAP_SDCARD_SPI_PIN_CS)
    mask |= 1ULL << CONFIG_SPANGAP_SDCARD_SPI_PIN_CS;
#endif
    gpio_config_t cs = {};
    cs.pin_bit_mask = mask;
    cs.mode         = GPIO_MODE_OUTPUT;
    cs.pull_up_en   = GPIO_PULLUP_DISABLE;
    cs.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cs.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cs);
    gpio_set_level((gpio_num_t)BOARD_IMU_CS_PIN, 1);
#if defined(CONFIG_SPANGAP_SDCARD_SPI_PIN_CS)
    gpio_set_level((gpio_num_t)CONFIG_SPANGAP_SDCARD_SPI_PIN_CS, 1);
#endif
}

/* =========================================================================
 * 3. The GNSS supply, for spangap/gps
 * ========================================================================= */

#if CONFIG_STRADDLE_GPS
/* gps declares this weak and calls it where a board owns the receiver's supply.
 * Here that switch is ALDO4 — a PMU register, not a pin, which is exactly the
 * case CONFIG_GPS_POWER_PIN cannot express. Cutting it leaves V_BCKP (from the
 * 18650) alive, so the receiver keeps its ephemeris and hot-starts when it
 * comes back; that is what CONFIG_GPS_POWER_KEEPS_BACKUP tells gps. */
extern "C" bool gpsBoardPower(bool on)
{
    if (!s_pmu) return false;
    if (on) pmuWrite(AXP2101_ALDO4_V, AXP2101_MV(3300));
    pmuRails(AXP2101_EN_ALDO4, on);
    return true;
}
#endif

/* =========================================================================
 * 4. QMI8658 IMU — straight back to sleep
 * ========================================================================= */
#if !CONFIG_STRADDLE_IMU

/* QMI8658 registers (SPI: address byte, bit7 = read). CTRL7 holds the per-
 * engine enables; CTRL1 bit 0 is SensorDisable, which stops the internal 2 MHz
 * oscillator — the part's lowest-power state.
 *
 * Bit 0, per the QMI8658C datasheet's CTRL1 table (rev 0.9 §5.4: bits 4:1 are
 * reserved). QST's own SensorLib sets bit 1 in its powerDown(), which by that
 * table writes a reserved bit and leaves the oscillator running; the register
 * definition is the authority here, so this writes bit 0. Read-modify-write,
 * because CTRL1 also carries the interface settings (BE defaults set, SIM
 * selects 4- vs 3-wire SPI) and this board wants those left alone. */
#define QMI8658_WHOAMI       0x00
#define QMI8658_WHOAMI_VAL   0x05
#define QMI8658_CTRL1        0x02
#define QMI8658_CTRL1_SLEEP  0x01   /* bit 0: SensorDisable */
#define QMI8658_CTRL7        0x08   /* bit 0 aEN, bit 1 gEN */

static bool imuXfer(spi_device_handle_t h, const uint8_t* tx, uint8_t* rx, size_t n)
{
    spi_transaction_t t = {};
    t.length    = n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(h, &t) == ESP_OK;
}

static uint8_t imuRead(spi_device_handle_t h, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0x00 }, rx[2] = {};
    return imuXfer(h, tx, rx, 2) ? rx[1] : 0xFF;
}

static void imuWrite(spi_device_handle_t h, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    imuXfer(h, tx, nullptr, 2);
}

/* Nothing in this build reads the IMU, and a part left in its power-on default
 * keeps its oscillator running for nobody. So the board says hello once and
 * puts it to sleep: engines off, then sensor-disable. The bus is opened and
 * freed here, because spangapInit()'s fs_mount_sd() claims this same host for
 * the card a moment later and two owners is one too many.
 *
 * The part answers on SPI mode 0 or mode 3 and which one a given unit likes is
 * not something the vendor's own code commits to, so the ID read decides: mode
 * 0 first, mode 3 if that returns nothing recognisable. */
static bool imuSleepAtMode(int mode)
{
    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 1000000;
    dev.mode           = mode;
    dev.spics_io_num   = BOARD_IMU_CS_PIN;
    dev.queue_size     = 1;
    spi_device_handle_t h = nullptr;
    if (spi_bus_add_device(BOARD_PERIPH_SPI_HOST, &dev, &h) != ESP_OK) return false;

    bool found = false;
    for (int i = 0; i < 5 && !found; i++) {          /* the part needs a few ms off a cold rail */
        found = imuRead(h, QMI8658_WHOAMI) == QMI8658_WHOAMI_VAL;
        if (!found) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (found) {
        imuWrite(h, QMI8658_CTRL7, 0x00);           /* accelerometer + gyroscope off */
        uint8_t c1 = imuRead(h, QMI8658_CTRL1);
        imuWrite(h, QMI8658_CTRL1, (uint8_t)(c1 | QMI8658_CTRL1_SLEEP));
    }
    spi_bus_remove_device(h);
    return found;
}

static void tbeamSupremeImuSleep(void)
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = BOARD_PERIPH_SPI_SCK;
    bus.mosi_io_num     = BOARD_PERIPH_SPI_MOSI;
    bus.miso_io_num     = BOARD_PERIPH_SPI_MISO;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 8;
    if (spi_bus_initialize(BOARD_PERIPH_SPI_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        warn("imu: peripheral SPI bus busy — IMU left as it woke up\n");
        return;
    }
    if (!imuSleepAtMode(0) && !imuSleepAtMode(3))
        warn("imu: no QMI8658 on CS%d — left as it woke up\n", BOARD_IMU_CS_PIN);
    spi_bus_free(BOARD_PERIPH_SPI_HOST);   /* hand the host to the SD driver */
}
#endif  /* !CONFIG_STRADDLE_IMU */

/* =========================================================================
 * Public API — the always-on board bring-up (see tbeamsupreme.h).
 * ========================================================================= */

void TbeamSupremeBoard::onStart() {
    tbeamSupremePmuInit();   /* rails on: SX1262, display/sensor bus, SD, GNSS */
    tbeamSupremeCsPark();    /* peripheral SPI bus: SD + IMU deselected */
#if !CONFIG_STRADDLE_IMU
    tbeamSupremeImuSleep();  /* nobody reads the IMU in this build — silence it */
    tbeamSupremeCsPark();    /* spi_bus_free() let both CS lines go — park again */
#endif
}
