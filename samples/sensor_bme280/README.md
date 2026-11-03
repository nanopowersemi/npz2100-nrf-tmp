# nPZ2100 BME280 sensor sample

Shows the nPZ2100 polling a Bosch BME280 temperature sensor on its own
while the nRF5x is powered off, waking the nRF5x on a temperature
threshold, and the periodic time-out (TOUT) working alongside it.

- **Every 2 s:** the nPZ2100 powers the BME280, starts one forced-mode
  temperature conversion and reads the result.
- **Start-up:** the thresholds are set to a window no real reading
  reaches, so the sensor does not wake the nRF5x.
- **Every 10 s (time-out):** the nRF5x wakes and prints the last
  temperature. On the first time-out the thresholds are changed to a
  0-40 °C window.
- **Sensor wake:** with the 0-40 °C window, the next poll wakes the nRF5x,
  which prints the temperature. At room temperature every poll is inside
  the window, so this repeats every 2 s.

Change `APP_WAKE_MIN_C` and `APP_WAKE_MAX_C` in `src/main.c` for another
window.

## Wiring

| BME280 | Connected to |
|--------|--------------|
| SDA, SCL | The nPZ2100 I²C bus, shared with the nRF5x (Arduino SDA/SCL on the DKs) |
| VDD, VDDIO | nPZ2100 SW_LP1 |
| SDO | GND (I²C address 0x76) |
| CSB | VDDIO (selects I²C) |

For other wiring, change `BME280_I2C_ADDR` and `BME280_SW_LP` in
`src/npz2100_regmap.h` (and `BME280_ADDR` in `ci/test.py`).

## How it works

| What | Mechanism | Where |
|------|-----------|-------|
| 2 s polling | Peripheral 1: `PERP = 32` (62.5 ms steps), I²C read of 0xFA/0xFB as one 16-bit value (`SWPRREG = 1`) | `src/npz2100_regmap.h`, applied every boot |
| Conversion per poll | SRAM init command `ctrl_meas (0xF4) = 0x21` (temperature x1, forced mode), 5 ms wait before (start-up) and after (conversion) | SRAM written on start-up, `TCFGP`/`TWTP` in the regmap |
| Threshold wake | `MODP.CMOD = 1`: wake when the reading is inside THRUNP-THROVP | thresholds in `src/main.c` |
| 10 s wake | `SYSCFG2.TOUT_EXT = 0`, `TOUT = 160` | `src/npz2100_regmap.h` |
| Temperature | `npz2100_periph_read_value()`, converted with the sensor calibration (dig_T1-T3) | `src/main.c`, every wake |

The nPZ2100 compares raw readings, so `src/main.c` turns 0 °C and 40 °C
into raw thresholds with the sensor's calibration, read directly from the
BME280 over the shared bus. `IOCFG1.PSWA_S1` keeps the sensor powered
while the nRF5x is awake so it can do that.

The thresholds are left out of the regmap on purpose: the regmap is
applied on every boot and would otherwise put them back each time. The
application keeps no state of its own; whether the window has been armed is
read back from the threshold registers.

## Building

Open `samples/sensor_bme280/` in VS Code with the nRF Connect extension,
add a build configuration for `nrf52dk/nrf52832`, `nrf52833dk/nrf52833` or
`nrf52840dk/nrf52840` and build. The `npz2100` shield in
`npz2100_module/boards/shields/npz2100/` is selected in `CMakeLists.txt`,
so no extra build arguments are needed.

## Expected output

```
<inf> app: --- nPZ2100 BME280 sample boot ---
<inf> app: Start-up - thresholds set out of range, sensor will not wake
<inf> app: Returning to idle
...
<inf> app: --- nPZ2100 BME280 sample boot ---
<inf> npz2100:   Wake: periodic time-out
<inf> app: Global time-out wake
<inf> app: Temperature: 23.41 C (raw 0x8155)
<inf> app: Thresholds set to 0-40 C window (raw 0x6B70-0x8A91)
<inf> app: Returning to idle
...
<inf> app: --- nPZ2100 BME280 sample boot ---
<inf> npz2100:   Wake: peripheral 1 triggered
<inf> app: Peripheral 1 wake - temperature in window
<inf> app: Temperature: 23.43 C (raw 0x8157)
<inf> app: Returning to idle
```

The raw values depend on the calibration of each sensor.
