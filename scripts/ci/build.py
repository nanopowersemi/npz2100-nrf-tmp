#!/usr/bin/env python3
"""Build one sample, or all of them, for one board.

    python scripts/ci/build.py --sample watchdog
    python scripts/ci/build.py --board nrf52840dk/nrf52840

The board defaults to NPZ_CI_BOARD, else nrf52dk/nrf52832. Each sample is
built to build/ci/<sample>, where flash.py and run_samples.py look for it.
"""

import argparse
import os

from ci_utils import REPO_ROOT, SAMPLES_DIR, build


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--board", default=os.environ.get("NPZ_CI_BOARD") or "nrf52dk/nrf52832")
    p.add_argument("--sample", action="append",
                   help="sample to build (repeatable); default: every sample")
    args = p.parse_args()

    names = args.sample or sorted(d.name for d in SAMPLES_DIR.iterdir()
                                  if (d / "CMakeLists.txt").is_file())
    for name in names:
        build(SAMPLES_DIR / name, args.board, REPO_ROOT / "build" / "ci" / name)

    print()
    print("Build successful!", flush=True)


if __name__ == "__main__":
    main()
