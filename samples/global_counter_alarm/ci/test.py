#!/usr/bin/env python3
"""Hardware test for the global counter alarm sample.

Run by scripts/ci/run_samples.py, or on its own with the same arguments:
    python samples/global_counter_alarm/ci/test.py --board <board> --port <port>
"""

# The alarm fires 30 s after the first boot; leave room for the flash, boot
# time and one 5 s time-out wake after it.
CAPTURE_S = 40


def check(log):
    log.expect_in_order(
        r"nPZ2100 global counter alarm sample boot",
        r"Counter: \d+\.\d{3} s",
        r"Returning to idle",
        r"Global counter alarm fired",
        r"alarm off",
    )

    # With SW_HP wired, every wake prints the counter: it must keep counting.
    counts = [int(m.group(1)) for m in log.find(r"Counter: (\d+)\.\d{3} s")]
    for prev, cur in zip(counts, counts[1:]):
        if cur <= prev:
            log.failures.append(f"counter did not advance: {prev} s -> {cur} s")


if __name__ == "__main__":
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts" / "ci"))
    from run_samples import main

    sys.exit(main(["--sample", Path(__file__).resolve().parents[1].name] + sys.argv[1:]))
