#!/usr/bin/env python3
"""Switch the AD3 supply off and reset the AD3, releasing all outputs."""

from ad3_utils import Ad3_handler

ad3 = Ad3_handler()
ad3.set_supply_voltage(0.0)
ad3.close(True)
print("AD3 supply off", flush=True)
