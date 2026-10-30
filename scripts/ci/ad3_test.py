#!/usr/bin/env python3
"""AD3 smoke test: read scope channel 1 and count DIO0-2 through 0..7."""

import time

from ad3_utils import Ad3_handler

ad3 = Ad3_handler()
print("Voltage: " + str(ad3.read_scope_voltage()), flush=True)
for i in range(0, 50):
    ad3.set_pins(i)
    time.sleep(0.05)
time.sleep(1)
ad3.close()
