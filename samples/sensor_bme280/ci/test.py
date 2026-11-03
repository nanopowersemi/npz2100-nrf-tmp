#!/usr/bin/env python3
"""Hardware test for the BME280 sensor sample.

Run by scripts/ci/run_samples.py, or on its own with the same arguments:
    python samples/sensor_bme280/ci/test.py --board <board> --port <port>
"""

import re

# Start-up, the first 10 s time-out wake, the sensor wake at the next 2 s
# poll, and a few more 2 s sensor wakes after it.
CAPTURE_S = 20

# Must match BME280_I2C_ADDR in src/npz2100_regmap.h.
BME280_ADDR = 0x76
POLL_PERIOD_S = 2.0
# The nPZ2100 timer accuracy varies from chip to chip.
POLL_PERIOD_TOL = 0.40  # +/- 40 %

# The bench is indoors: anything outside this is a bad reading or conversion.
TEMP_RANGE_C = (5.0, 40.0)

TEMP = re.compile(r"Temperature: (-?\d+\.\d{2}) C")


def check(log):
    log.expect_in_order(
        r"nPZ2100 BME280 sample boot",
        r"Start-up - thresholds set out of range",
        r"Returning to idle",
        r"Global time-out wake",
        r"Temperature: -?\d+\.\d{2} C",
        r"Thresholds set to 0-40 C window",
        r"Returning to idle",
        r"Peripheral 1 wake",
        r"Temperature: -?\d+\.\d{2} C",
    )

    # Walk the boots: no temperature before the first wake, no sensor wake
    # while the thresholds are out of range, and a temperature on every wake.
    armed = False
    wake = None  # the wake of the current boot, until its temperature
    for line in log.lines:
        if "BME280 sample boot" in line:
            if wake:
                log.failures.append(f"no temperature printed on the {wake}")
            wake = None
        elif "Global time-out wake" in line:
            wake = "time-out wake"
        elif "Peripheral 1 wake" in line:
            if not armed:
                log.failures.append("sensor woke the nRF before the thresholds were set to 0-40 C")
            wake = "sensor wake"
        elif "Thresholds set to" in line:
            armed = True
        elif (m := TEMP.search(line)):
            if not wake:
                log.failures.append(f"temperature printed on start-up: '{line.strip()}'")
            t = float(m.group(1))
            if not TEMP_RANGE_C[0] <= t <= TEMP_RANGE_C[1]:
                log.failures.append(f"temperature {t} C outside {TEMP_RANGE_C[0]}-{TEMP_RANGE_C[1]} C")
            wake = None

    # The thresholds are moved once, on the first time-out wake.
    arms = len(log.find(r"Thresholds set to"))
    if arms != 1:
        log.failures.append(f"expected the thresholds to be set once, saw {arms}")

    check_polling(log)


def check_polling(log):
    """With the I2C sniffer on, the nPZ2100 polls the BME280 every 2 s.

    Each poll starts with the SRAM init command, ctrl_meas (0xF4) = 0x21,
    which only the nPZ2100 writes, so those writes time the polls."""
    if not log.i2c:
        return
    polls = [t.time for t in log.i2c
             if t.addr == BME280_ADDR and t.register_access() == ("write", 0xF4, b"\x21")]
    if len(polls) < 3:
        log.failures.append(f"expected the nPZ2100 to poll the BME280 every "
                            f"{POLL_PERIOD_S} s, saw {len(polls)} polls")
        return
    gaps = sorted(b - a for a, b in zip(polls, polls[1:]))
    median = gaps[len(gaps) // 2]
    offset = median - POLL_PERIOD_S
    print(f"BME280 poll period {median:.2f} s (expected {POLL_PERIOD_S} s, "
          f"offset {offset:+.2f} s / {offset / POLL_PERIOD_S:+.1%}, "
          f"tolerance +/-{POLL_PERIOD_TOL:.0%})", flush=True)
    if abs(offset) > POLL_PERIOD_S * POLL_PERIOD_TOL:
        log.failures.append(f"BME280 poll period {median:.2f} s, expected {POLL_PERIOD_S} s "
                            f"+/- {POLL_PERIOD_TOL:.0%}")


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts" / "ci"))
    from run_samples import main

    sys.exit(main(["--sample", Path(__file__).resolve().parents[1].name] + sys.argv[1:]))
