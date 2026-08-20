/**
 * tbeamsupreme.h — LilyGo T-Beam S3 Supreme board support for reticulous.
 *
 * The Supreme is an ESP32-S3 LoRa+GNSS node on the ESP32-S3FN8 (8 MB flash,
 * 8 MB *quad* PSRAM) with an SX1262, a 1.3" SH1106 OLED, a microSD slot and an
 * AXP2101 PMU. Wired: the PMU rails, LoRa, microSD, the OLED as a paged status
 * display via spangap/tinylcd, the GNSS receiver via spangap/gps and the
 * PCF8563 via spangap/spangap-rtc (all four's pins published in
 * straddle.yaml). The QMI8658 IMU, QMC6310 magnetometer and BME280 stay
 * unwired. See tbeamsupreme.cpp for the implementation and the board
 * reference: https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series
 * (docs/en/t_beam_supreme/t_beam_supreme_hw.md).
 *
 * What this module provides:
 *   - Compile-time constants for the board-owned PMU bus and the peripheral
 *     SPI bus's IMU chip select.
 *   - The always-on board bring-up entry point TbeamSupremeBoard::onStart().
 *
 * The SX1262's pins, the SD pins, the OLED pins, the GNSS UART and the RTC's
 * bus are NOT wired here: they belong to iface-lora's CONFIG_LORA*,
 * spangap-core's CONFIG_SPANGAP_SDCARD_*, tinylcd's CONFIG_TINYLCD_*, gps's
 * CONFIG_GPS_* and spangap-rtc's CONFIG_RTC_* knobs, set as board VALUES in
 * this straddle's straddle.yaml `kconfig:` block. This header carries only
 * what no other straddle owns.
 */
#pragma once

#include "sdkconfig.h"
#include "service.h"

#include "driver/spi_master.h"   /* SPI3_HOST, for the peripheral bus below */

#define BOARD_NAME              "LilyGo T-Beam S3 Supreme"

/* AXP2101 PMU, on the board's second I2C bus together with the PCF8563 RTC —
 * separate from the OLED/sensor bus tinylcd uses. The SX1262 (ALDO3), display
 * + BME280 + magnetometer (ALDO1), the sensor rail the I2C bus itself needs
 * (ALDO2), GNSS (ALDO4) and microSD (BLDO1) hang off its rails and are DEAD
 * until it enables them — PMU bring-up must precede every other peripheral.
 * onStart creates this bus on controller 0 and spangap-rtc adopts it there
 * (CONFIG_RTC_I2C_PORT) to reach the RTC. */
#define BOARD_PMU_SDA_PIN       42
#define BOARD_PMU_SCL_PIN       41
#define BOARD_PMU_I2C_ADDR      0x34
#define BOARD_RTC_I2C_ADDR      0x51   /* PCF8563, same bus (spangap-rtc's chip) */

/* The board's peripheral SPI bus: the microSD and the QMI8658 IMU, one CS
 * each. straddle.yaml publishes the same numbers to spangap-core as
 * CONFIG_SPANGAP_SDCARD_SPI_* (the card's driver is not this straddle's); the
 * board keeps its own names because it touches the bus before anyone else
 * does — to park both CS lines, and to put the IMU to sleep. */
#define BOARD_PERIPH_SPI_HOST   SPI3_HOST
#define BOARD_PERIPH_SPI_SCK    36
#define BOARD_PERIPH_SPI_MOSI   35
#define BOARD_PERIPH_SPI_MISO   37
#define BOARD_IMU_CS_PIN        34

/* The GNSS receiver's lines — data pair and the L76K FORCE_ON wake input — are
 * spangap/gps's, published as CONFIG_GPS_* in straddle.yaml. This board only
 * switches their ALDO4 rail. */

/**
 * Board bring-up, as a registered Service. TbeamSupremeBoard::onStart is the
 * always-on hardware bring-up: it switches the AXP2101 rails for the SX1262,
 * the display/sensor bus, the microSD and — in a build carrying gps — the GNSS
 * receiver on, then parks the microSD and IMU CS lines of the shared
 * peripheral SPI bus HIGH so neither drives MISO before its driver claims it.
 * It runs in the start band, before
 * spangapInit() (and so before fs_mount_sd() probes that bus). There is no
 * onInit companion — the OLED UI, the GNSS reader and the clock are their own
 * straddles' services, not board hooks.
 */
class TbeamSupremeBoard : public Service {
public:
    void onStart() override;
};
