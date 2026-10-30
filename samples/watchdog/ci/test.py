#!/usr/bin/env python3
"""Hardware test for the watchdog sample.

Run by scripts/ci/run_samples.py, or on its own with the same arguments:
    python samples/watchdog/ci/test.py --board <board> --port <port>
"""

# Start-up, 3 s to the time-out wake, about 6 s hung, the watchdog reset
# boot, and a few normal 3 s wakes after it.
CAPTURE_S = 20

# TODO: re-enable once the nPZ2100 watchdog issue is fixed in hardware.
# While SKIP is set, the sample is still built but not flashed or tested,
# and the CI run shows a warning. Remove SKIP to re-enable the test, or
# pass --no-skip to run it anyway.
SKIP = "Disabled because of a suspected nPZ2100 watchdog hardware issue."


def check(log):
    log.expect_in_order(
        r"nPZ2100 watchdog sample boot",
        r"Start-up - hang on the next time-out wake",
        r"Time-out wake",
        r"Simulating a hang",
        r"Hung for \d+ s",
        r"Recovered from a watchdog reset",
        r"Time-out wake",
    )

    # The hang is shown once: the watchdog reset must not be followed by another.
    hangs = list(log.find(r"Simulating a hang"))
    if len(hangs) != 1:
        log.failures.append(f"expected one simulated hang, saw {len(hangs)}")


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts" / "ci"))
    from run_samples import main

    sys.exit(main(["--sample", Path(__file__).resolve().parents[1].name] + sys.argv[1:]))
