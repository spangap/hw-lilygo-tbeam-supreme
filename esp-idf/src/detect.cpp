/**
 * detect.cpp — is the hardware under this firmware a LilyGo T-Beam S3 Supreme
 * with an SX1262?
 *
 * The board's one self-assertion. It answers about THIS board only: its own
 * name when the PMU and the radio both answer on Supreme pins, and NULL
 * otherwise. Nothing here enumerates other boards — that comparison belongs to
 * whoever calls it.
 *
 * Two callers, one body:
 *
 *   * spangap-core, at the top of spangapInit(), before any bus is claimed. A
 *     board straddle is staged because the image was built for that board, so a
 *     NULL here means the image is on the wrong hardware and the platform halts
 *     rather than driving someone else's pins for the rest of the boot.
 *   * flashmon's standalone detector, which carries a copy of this function
 *     renamed `detect_hw_lilygo_tbeam_supreme` and calls it alongside every
 *     other board's, to identify a chip whose firmware is unknown.
 *
 * The copy is manual and deliberately so — see detect_probe.h. Change this,
 * change flashmon/esp-idf/main/detect.c.
 *
 * The rail: the SX1262 is dead until the AXP2101 turns ALDO3 on, so the probe
 * switches that rail itself — over I2C, not a GPIO, which is why the anchor has
 * to come first: an AXP2101 answering at 0x34 on 42/41 is what makes the write
 * safe to attempt. It restores the PMU's enable register ONLY when the probe
 * fails. On success the rail is wanted either way round — the firmware is about
 * to use it (and TbeamSupremeBoard::onStart drove it already, so this is
 * idempotent), and the detector is reset into real firmware straight after.
 *
 * The Supreme is a LINE: the same PCB ships with an SX1262, an LR1121 or an
 * SX1278 on identical pins, and each is its own straddle because the radio
 * driver differs. So this asks the radio which part it is and answers only for
 * the SX1262.
 */
#include "detect_probe.h"
#include "tbeamsupreme.h"

/* PMU registers, written out for the same reason the pins below are: this
 * probe runs in a detector that stages no straddles. */
#define DETECT_PMU_LDO_EN0   0x90
#define DETECT_PMU_ALDO1_V   0x92
#define DETECT_PMU_ALDO2_V   0x93
#define DETECT_PMU_ALDO3_V   0x94
#define DETECT_PMU_ALDO4_V   0x95
#define DETECT_PMU_EN_ALDO1  0x01
#define DETECT_PMU_EN_ALDO2  0x02
#define DETECT_PMU_EN_ALDO3  0x04
#define DETECT_PMU_EN_ALDO4  0x08
#define DETECT_PMU_MV(mv)    (uint8_t)(((mv) - 500) / 100)

/* LoRa header (straddle.yaml's CONFIG_LORA0_*, written out: those symbols only
 * exist when iface-lora is staged, and this must probe without it). */
#define DETECT_LORA_SCK   12
#define DETECT_LORA_MOSI  11
#define DETECT_LORA_MISO  13
#define DETECT_LORA_CS    10
#define DETECT_LORA_RST    5
#define DETECT_LORA_BUSY   4

/* The display/sensor bus and the GNSS RX pin, for the extras trace only. Their
 * rails are ALDO1+ALDO2 and ALDO4 (see tbeamsupreme.cpp). */
#define DETECT_OLED_SDA   17
#define DETECT_OLED_SCL   18
#define DETECT_GPS_RX      9

/** Set the named rails to 3.3 V and enable `bits` in the PMU's LDO enable
 *  register, keeping every other bit, and hand back what was there so a failed
 *  probe can put it back. Every rail on this board runs at 3.3 V. */
static bool detect_pmu_rails(const uint8_t* volt_regs, int n, uint8_t bits, uint8_t* saved)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, BOARD_PMU_SDA_PIN, BOARD_PMU_SCL_PIN)) return false;
    uint8_t en = 0;
    bool ok = detect_i2c_rd(&h, BOARD_PMU_I2C_ADDR, DETECT_PMU_LDO_EN0, &en, 1);
    if (ok) {
        if (saved) *saved = en;
        for (int i = 0; i < n; i++)
            detect_i2c_wr(&h, BOARD_PMU_I2C_ADDR, volt_regs[i], DETECT_PMU_MV(3300));
        ok = detect_i2c_wr(&h, BOARD_PMU_I2C_ADDR, DETECT_PMU_LDO_EN0, (uint8_t)(en | bits));
    }
    detect_i2c_close(&h);
    return ok;
}

static void detect_pmu_restore(uint8_t en)
{
    detect_i2c_t h;
    if (!detect_i2c_open(&h, BOARD_PMU_SDA_PIN, BOARD_PMU_SCL_PIN)) return;
    detect_i2c_wr(&h, BOARD_PMU_I2C_ADDR, DETECT_PMU_LDO_EN0, en);
    detect_i2c_close(&h);
}

extern "C" const char* detect_hw(void)
{
    /* 8 MB flash or it is not a Supreme — cheapest possible rejection, and it
     * touches no pin at all. */
    if (!detect_flash_mb(8)) return NULL;

    /* Anchor: the AXP2101 PMU on its own I2C bus. A plain ACK is all it needs
     * to give here — its meaning comes from the pins, which no other board
     * spangap knows uses for I2C. It is also what licenses the register write
     * below. */
    if (!detect_ack(BOARD_PMU_SDA_PIN, BOARD_PMU_SCL_PIN, BOARD_PMU_I2C_ADDR)) {
        detect_miss("no PMU at 0x%02X on %d/%d — not a T-Beam Supreme",
                    BOARD_PMU_I2C_ADDR, BOARD_PMU_SDA_PIN, BOARD_PMU_SCL_PIN);
        return NULL;
    }

    static const uint8_t radio_rail[] = { DETECT_PMU_ALDO3_V };
    uint8_t saved = 0;
    if (!detect_pmu_rails(radio_rail, 1, DETECT_PMU_EN_ALDO3, &saved)) {
        detect_miss("PMU answered but would not switch the radio rail");
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));             /* 3.3 V rail settle */

    /* Which Supreme: the radio names the straddle here, so nothing but an
     * SX1262 is this board. */
    if (!detect_radio_is(DETECT_LORA_SCK, DETECT_LORA_MOSI, DETECT_LORA_MISO,
                         DETECT_LORA_CS, DETECT_LORA_RST, DETECT_LORA_BUSY, "sx1262")) {
        detect_miss("T-Beam Supreme PMU, but the radio is not an SX1262");
        detect_pmu_restore(saved);
        return NULL;
    }

#if DETECT_EXTRAS
    /* Fitted either way round, so they confirm nothing and are logged for the
     * person reading: the PCF8563 RTC shares the PMU bus, and the display,
     * magnetometer and BME280 sit on the 17/18 bus behind ALDO1+ALDO2. The GNSS
     * receiver needs ALDO4 and its autobaud listens 1.2 s per rate, which is why
     * this never runs in the firmware — see DETECT_EXTRAS. */
    static const uint8_t extra_rails[] = { DETECT_PMU_ALDO1_V, DETECT_PMU_ALDO2_V,
                                           DETECT_PMU_ALDO4_V };
    detect_ack(BOARD_PMU_SDA_PIN, BOARD_PMU_SCL_PIN, BOARD_RTC_I2C_ADDR);
    detect_pmu_rails(extra_rails, 3,
                     DETECT_PMU_EN_ALDO1 | DETECT_PMU_EN_ALDO2 | DETECT_PMU_EN_ALDO4, NULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    detect_ack2(DETECT_OLED_SDA, DETECT_OLED_SCL, 0x3C, 0x3D);
    detect_bme280(DETECT_OLED_SDA, DETECT_OLED_SCL, NULL);
    detect_gps(DETECT_GPS_RX, NULL);
#endif

    detect_found("hw_lilygo_tbeam_supreme");
    return "hw-lilygo-tbeam-supreme";
}
