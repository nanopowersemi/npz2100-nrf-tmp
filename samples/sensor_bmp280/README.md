# nPZ2100 BMP280 sensor sample

Shows the nPZ2100 polling a Bosch BMP280 pressure sensor on its own while
the nRF5x is powered off, waking the nRF5x on a pressure threshold, and
the periodic time-out (TOUT) working alongside it. It is the
`sensor_bme280` sample with air pressure in place of temperature.

- **Every 2 s:** the nPZ2100 powers the BMP280, starts one forced-mode
  conversion and reads the raw pressure.
- **Start-up:** the thresholds are set to a window no real reading
  reaches, so the sensor does not wake the nRF5x.
- **Every 10 s (time-out):** the nRF5x wakes and prints the last
  pressure. On the first time-out the thresholds are changed to a
  300-1100 hPa window, the BMP280's whole operating range.
- **Sensor wake:** with that window, the next poll wakes the nRF5x, which
  prints the pressure. Every real reading is inside the window, so this
  repeats every 2 s.

Change `APP_WAKE_MIN_HPA` and `APP_WAKE_MAX_HPA` in `src/main.c` for another
window, e.g. a few hPa around the local pressure to wake only on a change.

## Wiring

| BMP280 | Connected to |
|--------|--------------|
| SDA, SCL | The nPZ2100 I²C bus, shared with the nRF5x (Arduino SDA/SCL on the DKs) |
| VDD, VDDIO | nPZ2100 SW_LP1 |
| SDO | GND (I²C address 0x76) |
| CSB | VDDIO (selects I²C) |

For other wiring, change `BMP280_I2C_ADDR` and `BMP280_SW_LP` in
`src/npz2100_regmap.h` (and `BMP280_ADDR` in `ci/test.py`).

## How it works

| What | Mechanism | Where |
|------|-----------|-------|
| 2 s polling | Peripheral 1: `PERP = 32` (62.5 ms steps), I²C read of 0xF7/0xF8 as one 16-bit value (`SWPRREG = 1`) | `src/npz2100_regmap.h`, applied every boot |
| Conversion per poll | SRAM init command `ctrl_meas (0xF4) = 0x25` (pressure and temperature x1, forced mode), 7.7 ms wait before (start-up) and after (conversion, 6.4 ms max) | SRAM written on start-up, `TCFGP`/`TWTP` in the regmap |
| Threshold wake | `MODP.CMOD = 1`: wake when the reading is inside THRUNP-THROVP | thresholds in `src/main.c` |
| 10 s wake | `SYSCFG2.TOUT_EXT = 0`, `TOUT = 160` | `src/npz2100_regmap.h` |
| Pressure | `npz2100_periph_read_value()`, compensated with the sensor calibration and the temperature | `src/main.c`, every wake |

The nPZ2100 compares raw readings, so `src/main.c` turns the hPa limits
into raw thresholds with the sensor's calibration. A higher raw value is a
lower pressure, so 1100 hPa gives the under threshold and 300 hPa the
over threshold.

Bosch's pressure compensation needs the temperature (`t_fine`) as well,
and the nPZ2100 only reads the pressure. So on every wake the nRF5x reads
the calibration and runs one forced temperature conversion itself, directly
over the shared bus (`ctrl_meas = 0x21`, about 5 ms). `IOCFG1.PSWA_S1`
keeps the sensor powered while the nRF5x is awake so it can do that. The
raw thresholds are worked out at the temperature of the first time-out
wake, which matters little for a window this wide.

The thresholds are left out of the regmap on purpose: the regmap is
applied on every boot and would otherwise put them back each time. The
application keeps no state of its own; whether the window has been armed is
read back from the threshold registers.

## Building

Open `samples/sensor_bmp280/` in VS Code with the nRF Connect extension,
add a build configuration for `nrf52dk/nrf52832`, `nrf52833dk/nrf52833` or
`nrf52840dk/nrf52840` and build. The `npz2100` shield in
`npz2100_module/boards/shields/npz2100/` is selected in `CMakeLists.txt`,
so no extra build arguments are needed.

## Expected output

```
<inf> app: --- nPZ2100 BMP280 sample boot ---
<inf> app: Start-up - thresholds set out of range, sensor will not wake
<inf> app: Returning to idle
...
<inf> app: --- nPZ2100 BMP280 sample boot ---
<inf> npz2100:   Wake: periodic time-out
<inf> app: Global time-out wake
<inf> app: Pressure: 1006.53 hPa (raw 0x6559) at 23.41 C
<inf> app: Thresholds set to 300-1100 hPa window (raw 0x5829-0xCA87)
<inf> app: Returning to idle
...
<inf> app: --- nPZ2100 BMP280 sample boot ---
<inf> npz2100:   Wake: peripheral 1 triggered
<inf> app: Peripheral 1 wake - pressure in window
<inf> app: Pressure: 1006.50 hPa (raw 0x6559) at 23.42 C
<inf> app: Returning to idle
```

The raw values depend on the calibration of each sensor.
