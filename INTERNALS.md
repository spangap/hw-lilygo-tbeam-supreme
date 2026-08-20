# hw-lilygo-tbeam-supreme internals

Design notes for maintainers. User-facing behaviour and the pin map live in
[README.md](README.md).

## The PMU-first rule

Everything interesting on this board — SX1262 (ALDO3), display + BME280 +
magnetometer (ALDO1), the sensor rail the display bus needs (ALDO2), microSD
(BLDO1), GNSS (ALDO4) — is powered from AXP2101 rails that come up
**disabled**. Any driver that touches its peripheral before the PMU enables the
rail sees a dead chip and fails its probe, so `TbeamSupremeBoard::onStart`
enables the rails first, in the start band, before `spangapInit()` (SD probe)
and long before tinylcd's task (OLED init) or `loraInit()`. The 20 ms
post-enable settle is for the rail, mirroring the V4's Vext settle.

**ALDO2 is not optional and is easy to miss**: it powers the sensor side of the
17/18 I2C bus (and the PCF8563), and LilyGo's hardware doc says plainly that
with it off "the I2C access will fail or freeze". A build that enables only
ALDO1 gets a display that sometimes works and a bus that sometimes hangs.

The rail map is the PowerManage table of LilyGo's `t_beam_supreme_hw.md`, and
Meshtastic's `Power.cpp` (`LILYGO_TBEAM_S3_CORE` branch) sets the same rails to
the same 3.3 V. DC1 feeds the ESP32-S3 itself and lives in another register
entirely, so nothing here can reach it. DC3/DC4/DC5 feed only the external M.2
socket and BLDO2 only a pin header — both vendors switch them on regardless;
this straddle leaves them off, because a rail with nothing on it is drain on a
battery board. Plugging something into that socket is what should turn them on.

The bring-up writes AXP2101 registers directly (voltage regs 0x92..0x96 =
(mV−500)/100, enable ctrl 0x90 bits ALDO1..4 = 0..3, BLDO1 = 4) instead of
pulling in a PMU driver: the board needs four rails on at 3.3 V, nothing else.
Charging config, fuel gauge, the PWR button and the IRQ line (GPIO 40) are all
PMU features deliberately left for whoever first needs them — at which point
the raw writes here should migrate into that driver, not coexist with it.

Every write to 0x90 is a read-modify-write on purpose: the register holds rails
this board deliberately keeps off, and a blind write would decide their fate
too.

## The cold-boot rail cycle

On a cold boot `onStart` powers ALDO1, ALDO2 and BLDO1 **down** for 250 ms
before enabling everything. The PMU keeps its rail state across an ESP32 reset,
so a card or a sensor that was mid-transaction when the previous firmware died
comes back still holding its bus — the SD holding MISO, an I2C device holding
SDA — and no bus reset from the ESP32 side clears that. A power cycle does.
LilyGo's own board support does exactly this ("In order to avoid bus
occupation…"), and Meshtastic carries it too. A wake from sleep skips the cycle:
those rails were ours the whole time, and the blink would cost an SD remount.

## Two SPI buses

The radio has **its own** bus (host 2: SCK 12 / MOSI 11 / MISO 13, CS 10) and
the microSD shares the **peripheral** bus (host 3: SCK 36 / MOSI 35 / MISO 37,
CS 47) with the QMI8658 IMU (CS 34). This is the T3-S3's dedicated-radio-bus
arrangement, not the T-Deck's shared one.

Consequence: the park in `onStart` is about the **peripheral** bus, not the
radio. Nothing in this build drives the IMU, so its CS would float, and a
floating CS is all it takes for the IMU to answer onto MISO in the middle of
the card's initialisation — which the SD probe in `spangapInit()` reads as a
card that will not talk. Both CS lines go out HIGH in one `gpio_config` call;
the SD half is `#if`-gated on `CONFIG_SPANGAP_SDCARD_SPI_PIN_CS` because the
symbol only exists when the card is configured in.

## The IMU: the straddle drives it, or the board silences it

With [imu](../imu) staged — the default — the QMI8658 belongs to that straddle,
which arms Wake-on-Motion and publishes whether the device is moving. The board
only publishes its pins and parks its CS.

`--without spangap/imu` leaves nobody reading the part, and one left at its
power-on default keeps its 2 MHz oscillator running for nobody. So under
`#if !CONFIG_STRADDLE_IMU` the board does it itself: open the peripheral SPI
bus, check WHO_AM_I, clear CTRL7 (accelerometer and gyroscope engines off), set
CTRL1's SensorDisable bit, then **free the host** — `fs_mount_sd()` claims that
same SPI host for the card moments later, and two owners is one too many. It
parks the CS lines again afterwards, because `spi_bus_free()` hands the pins
back.

Two details that will bite anyone extending this:

- **SensorDisable is CTRL1 bit 0**, per the datasheet's register table (bits
  4:1 are reserved). QST's SensorLib writes bit 1 in `powerDown()`, which by
  that table does nothing to the oscillator. Trust the register table.
- **The part answers on SPI mode 0 or mode 3** and the vendor code doesn't
  commit to which a given unit wants, so the bring-up tries mode 0, and mode 3
  if the ID read comes back unrecognisable. A unit that answers on neither
  leaves a warning and an IMU in whatever state it woke up in — which is the
  honest failure, since the board can't tell an absent part from a mute one.

Either way the same two details bite anyone extending this — they are why the
straddle exists rather than each board hand-rolling the part.

## I2C topology

Two buses, deliberately separate owners:

- **PMU bus** (SDA 42 / SCL 41): created by this straddle in onStart —
  single-threaded, so no creation race — and kept (handle cached) for a
  future PMU driver. The port is named, **0**, not auto-allocated, because the
  PCF8563 at 0x51 hangs off the same two wires: spangap-rtc adopts this bus by
  port number (`CONFIG_RTC_I2C_PORT=0`, its shared-bus mode) instead of
  creating a second master on them. That mode exists for this board — it was
  the first hardware to need it. The board must therefore keep controller 0,
  and tinylcd, which auto-allocates, gets the other one.
- **Display/sensor bus** (SDA 17 / SCL 18): owned by tinylcd, which creates its
  own bus on the pins this straddle publishes. The S3 has exactly two I2C
  controllers, so both auto-allocations succeed; a third bus user means
  moving to the T-Deck's shared-accessor pattern.

The magnetometer and BME280 share the display bus. When one of them gets wired,
tinylcd's bus ownership becomes the blocker — see the I2C section of
[tinylcd/INTERNALS.md](../tinylcd/INTERNALS.md).

## The GNSS rail is conditional, the others are not

ALDO4 comes on only under `CONFIG_STRADDLE_GPS` — a receiver nothing reads is
milliamps off an 18650 for no answer. That makes the rail a property of the
staged set rather than of the board, which is why it is a `#if` in onStart and
not a line in the rail block above it. `--without spangap/gps` therefore
produces a build with the receiver dark and no UART claimed.

The rail is also a **power switch**, which most boards do not give this
straddle: `gpsBoardPower()` here switches ALDO4 over the PMU, and the board
declares `CONFIG_GPS_POWER_KEEPS_BACKUP=y` because V_BCKP comes from the 18650,
so a cut supply leaves the receiver in hardware backup with its ephemeris
intact. That is what makes the "Unpower when off" row offerable here and absent
elsewhere. It is used only for *off* — never inside the duty cycle, and never
by the motion assist, which parks the receiver with the rail still up precisely
so the return is a hot start.

The board also publishes the L76K's FORCE_ON line (GPIO 7) as
`CONFIG_GPS_FORCE_PIN`, and gps pulses it on enable. That pin is the only exit
from the deep backup gps puts an L76K into, so before this board existed the
straddle could only report "power-cycle to wake"; the pin is wired here, so it
wakes. The pulse is a pulse and not a level on purpose — FORCE_ON held high
forces full-power mode, which would leave nothing for standby to save. The
u-blox units leave the pad unconnected, so the pulse costs them nothing and
neither straddle needs to know which receiver is fitted.

## Pitfalls

- **The OLED address moves with the magnetometer.** 0x3C on QMC6310U/QMC6309
  units, 0x3D on QMC6310N ones, because the two parts share the bus and the
  display is strapped off the magnetometer's address. tinylcd talks to one
  fixed address (`CONFIG_TINYLCD_I2C_ADDR`, 0x3C here), so a QMC6310N board
  shows nothing until that is changed. Symptom: a working radio, a working SD,
  and a dark screen with no I2C error anywhere.
- **SH1106, not SSD1306.** The 1.3" panel has the 132-column RAM; u8g2's
  sh1106 profile handles the offset, but flashing a build configured for the
  wrong controller shifts the image 2 px and wraps the edge columns.
- **PSRAM is quad, not octal.** The chip is an ESP32-S3FN8 with 8 MB of
  QSPI PSRAM, so `SPIRAM_MODE_QUAD`. An octal build does not fall back — it
  fails to bring PSRAM up at all.
- **BOOT is the strap pin.** The button's runtime meanings (click = page,
  500 ms hold = screen off, press = wake) are all harmless, and ROM-loader
  entry happens from reset before the firmware runs — no conflict.
- **The clock has its own battery.** The PCF8563 block carries a backup cell
  (schematic `BAT1`, behind a 1N4148 and a 1 kΩ series resistor onto the chip's
  VDD), which is why a board with no 18650 fitted and no USB attached still
  comes up with the time it was given — ours adopted a factory-set time, and
  the adopt path only accepts a reading with the VL flag clear, i.e. an
  oscillator that never stopped. Note this is **not** the PMU's VBACKUP /
  button-battery charger, which LilyGo's rail table lists as unused and both
  vendor firmwares disable: different mechanism, which is why the rail table
  says nothing about it. Consequence for anything that cuts power: the wall
  clock survives a PMU shutdown, and so does an armed alarm — but the interrupt
  it raises lands on an ESP32 that the same shutdown switched off, so only the
  PWR button brings the board back.
- **Two 32.768 kHz sources, and they are not the same thing.** The PCF8563 has
  its own watch crystal and keeps wall time across a power cut; the crystal on
  the S3's XTAL_32K pins (15/16) is the SoC's RTC slow clock, selected here
  with `CONFIG_RTC_CLK_SRC_EXT_CRYS`, and it only measures intervals while the
  chip is powered. Wanting one is not wanting the other. If the crystal fails
  to start IDF says so at boot and falls back to the internal RC oscillator —
  a working device with sleep timing suddenly measured in percent, so the
  warning is worth reading.
- **The IMU cannot wake this board from deep sleep.** Its interrupt is on
  GPIO 33, and the S3's RTC GPIOs stop at 21 — so no `ext0`/`ext1` source can
  see it. Light sleep can (ordinary GPIO wake works on any pin), and so can a
  polled read while awake. Anything that wants motion to end a deep sleep has
  to come from somewhere else: the PMU IRQ (GPIO 40) is equally out of range,
  the RTC alarm on GPIO 14 is not. Also unknown from the docs: whether GPIO 33
  is the part's INT1 or INT2, which Wake-on-Motion has to name — one shake with
  each setting on real hardware answers it.
- **PWR is the PMU's button, not a GPIO.** A 6-second hold cuts power at the
  PMU regardless of what the firmware wants, and the board comes back only on
  another press. It is not a reset the firmware can observe.
