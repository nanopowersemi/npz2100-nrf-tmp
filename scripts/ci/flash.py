#!/usr/bin/env python3
"""Flash a sample built by build.py to the bench board.

    python scripts/ci/flash.py --sample watchdog

Uses the J-Link given by NPZ_CI_DEV_ID if set.
"""

import argparse
import os

from ci_utils import REPO_ROOT, flash


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--sample", required=True)
    p.add_argument("--dev-id", default=os.environ.get("NPZ_CI_DEV_ID"))
    args = p.parse_args()

    flash(REPO_ROOT / "build" / "ci" / args.sample, args.dev_id)

    print()
    print("Flash successful!", flush=True)


if __name__ == "__main__":
    main()
