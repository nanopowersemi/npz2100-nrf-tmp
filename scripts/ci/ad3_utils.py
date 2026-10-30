"""Analog Discovery 3 helpers for the bench.

The AD3 positive supply (V+) powers the bench, and DIO0-2 are outputs (not
routed to the nRF). DIO4 receives the nRF UART TX, and DIO5 is wired to the
nRF reset (P0.21). Settings made here stay applied after the script exits
as long as close() is called without a device reset, so separate steps can
switch the supply on and off.

Environment:
    NPZ_CI_AD3_SN         AD3 serial number (optional, if more than one is attached)
    NPZ_CI_AD3_VOLTAGE    supply voltage in V (default 3.3)
    NPZ_CI_AD3_RESET_DIO  DIO wired to the nRF reset (default 5)
"""

import os
import time

import dwfpy as dwf

SUPPLY_V = float(os.environ.get("NPZ_CI_AD3_VOLTAGE") or 3.3)
NRF_RESET_DIO = int(os.environ.get("NPZ_CI_AD3_RESET_DIO") or 5)


class Ad3_handler:
    pin_reset = 0

    def __init__(self, serial_number: str = None):
        if serial_number is None:
            serial_number = os.environ.get("NPZ_CI_AD3_SN") or None
        self.device = dwf.Device(serial_number=serial_number)
        self.device.open()
        # Configure output pins, set them high, and apply the config
        self.device.digital_io[0].setup(enabled=True, state=True, configure=True)
        self.device.digital_io[1].setup(enabled=True, state=True, configure=True)
        self.device.digital_io[2].setup(enabled=True, state=True, configure=True)

    def set_reset(self, pinstate):
        self.device.digital_io[self.pin_reset].output_state = pinstate

    def set_pins(self, counter):
        self.device.digital_io[0].output_state = counter % 2
        self.device.digital_io[1].output_state = (counter // 2) % 2
        self.device.digital_io[2].output_state = (counter // 4) % 2

    def read_scope_voltage(self, channel: int = 0) -> float:
        """Reads a single instantaneous voltage sample from a scope input channel."""
        self.device.analog_input.read_status()
        return float(self.device.analog_input[channel].get_sample())

    def set_supply_voltage(self, voltage: float) -> None:
        """Sets the positive power supply output voltage. 0.0 disables the output."""
        supply_enable = self.device.analog_io[0][0]   # positive supply enable node
        supply_voltage = self.device.analog_io[0][1]  # positive supply voltage node

        if voltage == 0.0:
            supply_enable.value = False
        else:
            supply_voltage.value = voltage
            supply_enable.value = True

        self.device.analog_io.master_enable = True   # required in addition to the per-channel enable
        self.device.analog_io.configure()

    def hold_nrf_reset(self, hold: bool = True) -> None:
        """Pulls the nRF reset low, or releases it. Released means high-Z
        (the nRF has an internal pull-up): the line is never driven high, so
        it cannot back-power an unpowered nRF. A new handle starts released."""
        self.device.digital_io[NRF_RESET_DIO].setup(enabled=hold, state=False, configure=True)

    def uart_rx_setup(self, pin: int, rate: int = 115200) -> None:
        """Starts the UART receiver on DIO pin (8N1)."""
        self.device.protocols.uart.setup(pin_rx=pin, rate=rate)

    def uart_read(self) -> bytes:
        """Returns the bytes received since the last call. Overflow of the
        AD3 receive buffer is reported, since bytes have then been lost."""
        data, parity = self.device.protocols.uart.read()
        if parity < 0:
            print("AD3: UART receive buffer overflow, bytes lost", flush=True)
        return data

    def power_cycle(self, off_s: float = 1.0, voltage: float = SUPPLY_V) -> None:
        """Switches the supply off for off_s seconds, then back on."""
        self.set_supply_voltage(0.0)
        time.sleep(off_s)
        self.set_supply_voltage(voltage)

    def close(self, deviceReset = False):
        if not deviceReset:
            self.device.auto_reset = False
            self.device.set_parameter(dwf.GlobalParameter.ON_CLOSE, 0)
        self.device.close()

    # let the class itself be used as a context manager too
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
