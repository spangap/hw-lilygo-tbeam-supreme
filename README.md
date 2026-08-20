# hw-lilygo-tbeam-supreme — LilyGo T-Beam S3 Supreme board HAL

**hw-lilygo-tbeam-supreme** is the board-support straddle for the **LilyGo
T-Beam S3 Supreme** — an ESP32-S3 LoRa+GNSS node on the ESP32-S3FN8 (8 MB
flash, 8 MB **quad** PSRAM) carrying one Semtech **SX1262**, a 1.3" SH1106
OLED, a microSD slot, an L76K/u-blox M10 GNSS module, IMU/RTC/magnetometer/
BME280 sensors, an 18650 holder and an **AXP2101 PMU**. It makes the board
usable by an application: it owns the PMU rail bring-up and the peripheral-bus
CS park, and it publishes the board's pin map and hardware tuning as Kconfig.
Board reference: LilyGo's
[t_beam_supreme_hw.md](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t_beam_supreme/t_beam_supreme_hw.md),
cross-checked against Meshtastic's `variants/esp32s3/tbeam-s3-core` (the same
board — Meshtastic's `tbeam-s3-core` env is displayed as "LILYGO T-Beam
Supreme").

It is a **non-buildable** component — it decides nothing about what the device
*does*. A buildable assembler (`reticulous/reticulous`) adds it and inherits
the board: `spangap build reticulous/reticulous --with
spangap/hw-lilygo-tbeam-supreme`.

Wired: the **PMU rails**, **LoRa**, **microSD**, the **1.3" SH1106 OLED** as a
paged status display via [tinylcd](../tinylcd) (the BOOT button advances the
page), the **GNSS receiver** via [gps](../gps) and the **PCF8563 clock** via
[spangap-rtc](../spangap-rtc). The last three are staged by this board through
`additional_installs`, with their pins fed through gated `kconfig:` groups. The
IMU, magnetometer and BME280 stay unwired.

## This is the SX1262 board of a line

The units on sale differ only in GNSS receiver (L76K or u-blox), which is a
runtime question [gps](../gps) answers by autobauding. The radio is not: the
same PCB and the same pins carry an **SX1262**, an **LR1121**
(`[env:T_BEAM_S3_SUPREME_LR1121]` in LilyGo's platformio.ini, with its own
RadioLib examples and RF section in the hardware doc) or an **SX1278** (the
`t_beam_supreme_144_hw.md` board, which adds DIO0 on GPIO 2), and the radio
driver differs. So this straddle is the SX1262 one, the
[T3-S3](../hw-lilygo-t3s3-sx1262) arrangement, and `detect_hw()` asks the radio
which part it is — an LR1121 Supreme running this image halts at boot instead
of driving a radio that isn't there. A 433 MHz **SX1268** passes: it answers
the SX126x probe identically and iface-lora drives it the same way.

## What it does, and how it fits

| Hook | Band | Present when | Brings up |
|---|---|---|---|
| `TbeamSupremeBoard::onStart` | start | always | AXP2101 rails (SX1262, display/sensor bus, SD, and GNSS with `gps` staged) ON at 3.3 V, SD + IMU CS park HIGH |

`onStart` runs in the `start:` band, **before** `spangapInit()`. It is
bare-hardware bring-up in two steps, in this order:

1. **PMU rails.** The SX1262 (ALDO3), display + BME280 + magnetometer
   (ALDO1), the sensor rail that I2C bus needs (ALDO2) and the microSD
   (BLDO1) hang off AXP2101 rails and are **dead until the PMU enables
   them** — unlike the Heltec V4, where only Vext peripherals are gated and
   the radio is powered directly. Direct register writes on the PMU's own
   I2C bus (SDA 42 / SCL 41, address 0x34); no full PMU driver (charging,
   fuel gauge) until something needs it. On a cold boot the display/sensor
   and SD rails are power-cycled first (see [INTERNALS.md](INTERNALS.md)).
2. **CS park.** The microSD shares its SPI bus with the QMI8658 IMU, so
   **both** CS lines are parked HIGH before `fs_mount_sd()` probes that bus.
   The radio has a bus to itself and needs no park.
3. **IMU to sleep.** Nothing reads the QMI8658, so the board tells it to stop:
   engines off, oscillator disabled, then the SPI host is freed for the SD
   driver. Waking it for motion instead is a 30–55 µA proposition — see
   [INTERNALS.md](INTERNALS.md).
There is no `init:`-band companion — the OLED UI, the GNSS reader and the
clock are [tinylcd](../tinylcd)'s, [gps](../gps)'s and
[spangap-rtc](../spangap-rtc)'s own services, not board hooks.

## Hardware & pin map

### LoRa SX1262 (owned by iface-lora, pins published here)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| NSS / CS | 10 | | RST | 5 |
| SCK | 12 | | BUSY | 4 |
| MOSI | 11 | | DIO1 | 1 |
| MISO | 13 | | | |

Its **own** SPI bus (host 2) — the peripherals get the other one. The SX1262
drives **DIO2** as its own RF antenna switch and **DIO3** supplies the 1.8 V
TCXO (`CONFIG_LORA0_TCXO_MV=1800`). One radio (`CONFIG_LORA_COUNT=1`),
`CONFIG_LORA0_RADIO_SX1262=y`. Powered from ALDO3. **Connect the antenna
before transmitting** — LilyGo's first note on this board, and an SX1262 into
an open port damages easily.

### microSD (owned by spangap-core, pins published here)

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| SCK | 36 | | CS | 47 |
| MOSI | 35 | | (IMU CS) | 34 |
| MISO | 37 | | | |

The board's peripheral SPI bus (host 3), **shared with the QMI8658 IMU** on
its own CS. Powered from BLDO1. Mounted at boot when
`CONFIG_SPANGAP_SDCARD=y`.

### OLED + page button (owned by tinylcd, pins published here)

| Signal | GPIO | Notes |
|---|---|---|
| OLED SDA / SCL | 17 / 18 | 1.3" SH1106 128×64 (`CONFIG_TINYLCD_SH1106=y`), no reset line |
| page button (BOOT) | 0 | click = next status page; 500 ms hold = screen off; press wakes a dark screen |

Powered from ALDO1, with ALDO2 up as well — that bus does not answer with the
sensor rail off.

**The panel's address depends on which magnetometer the unit carries**, because
the two share the bus and the display is strapped away from the magnetometer's
address: **0x3C** with a QMC6310U or QMC6309, **0x3D** with a QMC6310N. This
straddle publishes `CONFIG_TINYLCD_I2C_ADDR=0x3C`; a QMC6310N board needs 0x3D
there. (V3.1 boards add a selection resistor for both the OLED and the BME280.)

### GNSS (owned by gps, pins published here)

| Signal | GPIO | Notes |
|---|---|---|
| host RX ← GNSS TX | 9 | NMEA in, UART1 |
| host TX → GNSS RX | 8 | commands out (standby, fix rate) |
| FORCE_ON / wake | 7 | `CONFIG_GPS_FORCE_PIN`; L76K units only — gps pulses it to wake a receiver it put into deep backup |
| PPS | 6 | unwired (it also drives the PPS LED, which cannot be turned off) |

A Quectel **L76K** or a u-blox **MAX-M10** depending on the unit — [gps](../gps)
autobauds and names the one it found. Powered from ALDO4, which comes on only
in a build that stages `gps`. GPS **backup power comes from the 18650**, so hot
start needs a battery fitted, not just USB.

The receiver's supply is **ALDO4**, and the board switches it for `gps` through
`gpsBoardPower()` — a PMU register, which is the case `CONFIG_GPS_POWER_PIN`
cannot express. V_BCKP comes from the 18650, so a cut supply still hot-starts,
which is what `CONFIG_GPS_POWER_KEEPS_BACKUP=y` tells the straddle.

### IMU (owned by imu, pins published here)

The **QMI8658** on the peripheral SPI bus (CS 34, INT 33), which the
[imu](../imu) straddle holds in low power with Wake-on-Motion armed — 30–55 µA
depending on rate, against the ~7 mA the GNSS spends in cyclic tracking. That is
what pays for `s.gps.imu_assist`: the receiver parks while the device stands
still. The card's driver brings that SPI host up first, so the straddle adopts
it rather than initialising it.

Its interrupt is on GPIO 33, **outside the S3's RTC GPIO range (0–21)**, so it
cannot wake deep sleep — the straddle polls the part's latched status anyway, so
nothing here depends on it. Whether the board wired INT1 or INT2 is undocumented;
if the line never fires, `CONFIG_IMU_INT_LINE=2` is the other guess.

### RTC (owned by spangap-rtc, wiring published here)

The **PCF8563** at 0x51 sits on the PMU's I2C bus, powered from ALDO2. The
board creates that bus on controller 0 in `onStart`, and spangap-rtc adopts it
(`CONFIG_RTC_I2C_PORT=0`) rather than putting a second master on the same
wires. Its interrupt line (GPIO 14) is unwired.

### Board-owned pins (in this straddle's `tbeamsupreme.h`)

| Signal | GPIO | Notes |
|---|---|---|
| PMU SDA / SCL | 42 / 41 | AXP2101 at 0x34, sharing this bus with the PCF8563 (0x51) |
| PMU IRQ | 40 | present, unwired |
| peripheral SPI SCK / MOSI / MISO | 36 / 35 / 37 | the microSD's bus; published to spangap-core as `CONFIG_SPANGAP_SDCARD_SPI_*` |
| IMU CS | 34 | parked HIGH; the board uses it once, to put the QMI8658 to sleep |

### Present but NOT wired (for reference)

The QMI8658's data path (it is put to sleep at boot, never read; INT on 33),
the QMC6310/QMC6309 magnetometer and BME280 (0x77, or 0x76 with the resistor
moved) on the display bus, the RTC interrupt (14), the GNSS PPS line (6), the
PMU power button and IRQ, and the M.2 socket on DC3/4/5.

### Memory / flash (published from `kconfig:`)

| Key | Value | Why |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | `y` | 8 MB flash (ESP32-S3FN8) |
| `CONFIG_SPANGAP_MAX_FIRMWARE_KB` | `6144` | state floor at 6 MB: app+fixed plus growth headroom below it, `/state` keeps the remaining ~2 MB (bulk data belongs on the microSD). Without it `app` eats all 8 MB — leaving **no `/state`** |
| `CONFIG_SPIRAM_MODE_QUAD` | `y` | 8 MB **quad** PSRAM — the platform's usual octal assumption does NOT hold here (LilyGo: "8MB(Quad-SPI)"; their Arduino setup says QSPI PSRAM) |
| `CONFIG_RTC_CLK_SRC_EXT_CRYS` | `y` | the board's 32.768 kHz crystal (S3 XTAL_32K pins 15/16) drives the RTC slow clock instead of the internal RC oscillator; IDF falls back with a boot warning if it doesn't start |

## Board identity (`detect_hw`)

`esp-idf/src/detect.cpp` answers one question about this board: it returns
`"hw-lilygo-tbeam-supreme"` when the hardware under it is a Supreme with an
SX1262, and NULL otherwise. It checks 8 MB of flash, ACKs the AXP2101 at 0x34
on 42/41 — the one bus write it is licensed by — switches ALDO3 on and then
requires the radio to identify itself as an SX1262. A failed probe puts the
PMU's enable register back the way it found it.

spangap-core calls it at the top of `spangapInit()`: a NULL means the image is
on the wrong hardware, and the platform halts rather than driving someone
else's pins. A confirmed board is announced on the console as
`build: hw hw-lilygo-tbeam-supreme`. flashmon's standalone detector carries a
hand-kept copy of the same function, renamed `detect_hw_lilygo_tbeam_supreme`,
to identify a chip whose firmware is unknown — change one, change the other.
The contract and the probe vocabulary are in
[flashmon/docs/detect.md](../flashmon/docs/detect.md).

## Storage variables

This board defines no storage keys of its own. Runtime LoRa parameters live at
`s.lora.*` ([iface-lora](../iface-lora)); the SD mount is owned by
[spangap-core](../spangap-core); the display at `s.tinylcd.*`
([tinylcd](../tinylcd)); the receiver at `s.gps.*` ([gps](../gps)); the clock
publishes `rtc.present` ([spangap-rtc](../spangap-rtc)).

## Dependencies

- [spangap-core](../spangap-core) — base runtime (storage, log, CLI, fs incl.
  the SD mount, ITS).
- [iface-lora](../iface-lora) — owns the SX1262 radio engine; this board
  powers its rail and supplies its pins via Kconfig.
- [tinylcd](../tinylcd) — staged by this board (`additional_installs`); owns
  the OLED paged UI and the page button, pins supplied via Kconfig.
- [gps](../gps) — staged by this board; owns the GNSS UART, the autobaud and
  the fix, pins supplied via Kconfig. Drop it with
  `--without spangap/gps` and the ALDO4 rail stays off with it.
- [spangap-rtc](../spangap-rtc) — staged by this board; owns the PCF8563,
  adopting the PMU bus this board creates.
- [imu](../imu) — staged by this board; owns the QMI8658 and publishes whether
  the device is moving. Dropped (`--without spangap/imu`), the board falls back
  to putting the part to sleep at boot and gps tracks continuously.

## Read next

- [INTERNALS.md](INTERNALS.md) — the PMU-first bring-up rule, the two SPI
  buses, and the board pitfalls.
