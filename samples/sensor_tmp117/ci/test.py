#!/usr/bin/env python3
"""Hardware test for the TMP117 sensor sample.

Run by scripts/ci/run_samples.py, or on its own with the same arguments:
    python samples/sensor_tmp117/ci/test.py --board <board> --port <port>
"""

# One boot: the sample waits 1 s before returning to idle.
CAPTURE_S = 20


def check(log):
    log.expect_in_order(
        r"nPZ2100 sample boot",
        r"Returning to idle",
    )


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts" / "ci"))
    from run_samples import main

    sys.exit(main(["--sample", Path(__file__).resolve().parents[1].name] + sys.argv[1:]))
