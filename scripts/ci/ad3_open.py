#!/usr/bin/env python3
"""Switch the AD3 supply on, and leave it on after exit."""

from ad3_utils import SUPPLY_V, Ad3_handler

ad3 = Ad3_handler()
ad3.set_supply_voltage(SUPPLY_V)
ad3.close()
print(f"AD3 supply on at {SUPPLY_V} V", flush=True)
