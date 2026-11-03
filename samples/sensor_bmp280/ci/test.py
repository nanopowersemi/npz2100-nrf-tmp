#!/usr/bin/env python3
"""Hardware test for the BMP280 sensor sample.

Run by scripts/ci/run_samples.py, or on its own with the same arguments:
    python samples/sensor_bmp280/ci/test.py --board <board> --port <port>
"""

import re

# Start-up, the first 10 s time-out wake, the sensor wake at the next 2 s
# poll, and a few more 2 s sensor wakes after it.
CAPTURE_S = 20

# Must match BMP280_I2C_ADDR in src/npz2100_regmap.h.
BMP280_ADDR = 0x76
POLL_PERIOD_S = 2.0
# The nPZ2100 timer accuracy varies from chip to chip.
POLL_PERIOD_TOL = 0.40  # +/- 40 %

# Air pressure on the bench: anything outside this is a bad reading or conversion.
PRESS_RANGE_HPA = (800.0, 1100.0)

PRESS = re.compile(r"Pressure: (\d+\.\d{2}) hPa")

# There is no BMP280 on the bench. While SKIP is set, the sample is built
# but not flashed or tested; pass --no-skip to run it on a bench with one.
# It also has no step in .github/workflows/samples.yml.
SKIP = "Disabled because there is no BMP280 on the bench."


def check(log):
    log.expect_in_order(
        r"nPZ2100 BMP280 sample boot",
        r"Start-up - thresholds set out of range",
        r"Returning to idle",
        r"Global time-out wake",
        r"Pressure: \d+\.\d{2} hPa",
        r"Thresholds set to 300-1100 hPa window",
        r"Returning to idle",
        r"Peripheral 1 wake",
        r"Pressure: \d+\.\d{2} hPa",
    )

    # Walk the boots: no pressure before the first wake, no sensor wake
    # while the thresholds are out of range, and a pressure on every wake.
    armed = False
    wake = None  # the wake of the current boot, until its pressure
    for line in log.lines:
        if "BMP280 sample boot" in line:
            if wake:
                log.failures.append(f"no pressure printed on the {wake}")
            wake = None
        elif "Global time-out wake" in line:
            wake = "time-out wake"
        elif "Peripheral 1 wake" in line:
            if not armed:
                log.failures.append("sensor woke the nRF before the thresholds were set to 300-1100 hPa")
            wake = "sensor wake"
        elif "Thresholds set to" in line:
            armed = True
        elif (m := PRESS.search(line)):
            if not wake:
                log.failures.append(f"pressure printed on start-up: '{line.strip()}'")
            p = float(m.group(1))
            if not PRESS_RANGE_HPA[0] <= p <= PRESS_RANGE_HPA[1]:
                log.failures.append(f"pressure {p} hPa outside {PRESS_RANGE_HPA[0]}-{PRESS_RANGE_HPA[1]} hPa")
            wake = None

    # The thresholds are moved once, on the first time-out wake.
    arms = len(log.find(r"Thresholds set to"))
    if arms != 1:
        log.failures.append(f"expected the thresholds to be set once, saw {arms}")

    check_polling(log)


def check_polling(log):
    """With the I2C sniffer on, the nPZ2100 polls the BMP280 every 2 s.

    Each poll starts with the SRAM init command, ctrl_meas (0xF4) = 0x25,
    which only the nPZ2100 writes (the nRF writes 0x21), so those writes
    time the polls."""
    if not log.i2c:
        return
    polls = [t.time for t in log.i2c
             if t.addr == BMP280_ADDR and t.register_access() == ("write", 0xF4, b"\x25")]
    if len(polls) < 3:
        log.failures.append(f"expected the nPZ2100 to poll the BMP280 every "
                            f"{POLL_PERIOD_S} s, saw {len(polls)} polls")
        return
    gaps = sorted(b - a for a, b in zip(polls, polls[1:]))
    median = gaps[len(gaps) // 2]
    offset = median - POLL_PERIOD_S
    print(f"BMP280 poll period {median:.2f} s (expected {POLL_PERIOD_S} s, "
          f"offset {offset:+.2f} s / {offset / POLL_PERIOD_S:+.1%}, "
          f"tolerance +/-{POLL_PERIOD_TOL:.0%})", flush=True)
    if abs(offset) > POLL_PERIOD_S * POLL_PERIOD_TOL:
        log.failures.append(f"BMP280 poll period {median:.2f} s, expected {POLL_PERIOD_S} s "
                            f"+/- {POLL_PERIOD_TOL:.0%}")


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts" / "ci"))
    from run_samples import main

    sys.exit(main(["--sample", Path(__file__).resolve().parents[1].name] + sys.argv[1:]))
