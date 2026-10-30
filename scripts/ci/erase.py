#!/usr/bin/env python3
"""Erase the bench nRF, so it is left without test firmware.

Erases the J-Link given by NPZ_CI_DEV_ID, or every connected device. With
NPZ_CI_AD3 set, the bench is first power-cycled with the nRF held in reset,
so the nPZ2100 is in standby and the nRF powered (see ad3_clean_erase).
"""

import os

from ci_utils import ad3_clean_erase, erase


def main():
    dev_id = os.environ.get("NPZ_CI_DEV_ID")
    if os.environ.get("NPZ_CI_AD3"):
        ad3_clean_erase(dev_id)
    else:
        erase(dev_id)

    print()
    print("Flash Erase successful!", flush=True)


if __name__ == "__main__":
    main()
