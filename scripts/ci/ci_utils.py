"""Shared helpers for the sample CI scripts.

Every sample that has a hardware test keeps it in samples/<name>/ci/test.py.
That module defines what to look for in the log; this file does the
building, flashing and serial capture that all samples share.
"""

import re
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SAMPLES_DIR = REPO_ROOT / "samples"
BAUDRATE = 115200

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def _echo(line):
    # Garbled bytes become U+FFFD, which a non-UTF-8 console cannot print.
    enc = sys.stdout.encoding or "utf-8"
    print(f"[UART] {line}".encode(enc, errors="replace").decode(enc), flush=True)


def run(command, cwd=REPO_ROOT, quiet=False):
    """Runs command, exiting on failure. With quiet, the output is held back
    and only printed if the command fails; it is returned either way."""
    print()
    print("==================================================", flush=True)
    print("Running:", " ".join(str(c) for c in command), flush=True)
    print("==================================================", flush=True)

    if quiet:
        result = subprocess.run([str(c) for c in command], cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
    else:
        result = subprocess.run([str(c) for c in command], cwd=cwd)

    if result.returncode != 0:
        if quiet:
            print(result.stdout, flush=True)
        print(f"Command failed with exit code {result.returncode}", flush=True)
        sys.exit(result.returncode)

    return result.stdout


def build(sample_dir, board, build_dir, quiet=False):
    """Builds a sample. With quiet, only the memory usage table is shown,
    unless the build fails."""
    out = run(["west", "build", "-p", "always", "-b", board, "-d", build_dir, sample_dir],
              quiet=quiet)
    if quiet:
        lines = _ANSI.sub("", out).splitlines()
        start = next((i for i, l in enumerate(lines) if l.startswith("Memory region")), None)
        if start is not None:
            end = start + 1
            while end < len(lines) and re.match(r"\s+\S+:\s+\d", lines[end]):
                end += 1
            print("\n".join(lines[start:end]), flush=True)
        print("Build successful", flush=True)


def flash(build_dir, dev_id=None, no_rebuild=False):
    cmd = ["west", "flash", "-d", build_dir]
    if no_rebuild:
        cmd += ["--no-rebuild"]
    if dev_id:
        cmd += ["--dev-id", dev_id]
    run(cmd)


def _erase_cmd(dev_id):
    cmd = ["nrfutil", "device", "erase", "--all"]
    if dev_id:
        cmd += ["--serial-number", dev_id]
    return cmd


def erase(dev_id=None):
    run(_erase_cmd(dev_id))


def _try_erase(dev_id):
    print("Running:", " ".join(_erase_cmd(dev_id)), flush=True)
    return subprocess.run(_erase_cmd(dev_id), cwd=REPO_ROOT).returncode == 0


def open_ad3():
    from ad3_utils import Ad3_handler  # dwfpy; imported here so builds work without it

    return Ad3_handler()


def ad3_clean_erase(dev_id=None, off_s=1.0, retries=3, ad3=None):
    """Erases the nRF from a clean bench start, for a bench where the nRF is
    powered from the nPZ2100 SW_HP and can only be programmed in standby.

    The nRF is held in reset (AD3 DIO5) while the AD3 supply is
    power-cycled, so no old firmware runs: the nPZ2100 comes out of its
    power-on reset in standby with SW_HP on and stays there. The erase is
    tried with reset held, then with it released. Reset is always released
    at the end, so the blank nRF can be flashed while the nPZ2100 waits in
    standby.

    Pass an open ad3 to keep using it afterwards (it is not closed), e.g.
    for the log capture: reopening the AD3 between the erase and the flash
    is avoided, so nothing can disturb the supply in between.
    """
    print()
    print(f"AD3: power-cycling the supply ({off_s} s off) with the nRF in reset", flush=True)
    own = ad3 is None
    if own:
        ad3 = open_ad3()
    try:
        ad3.hold_nrf_reset(True)
        ad3.power_cycle(off_s)
        time.sleep(0.5)  # let the supply settle and SW_HP come on

        if _try_erase(dev_id):
            print("Erased with the nRF held in reset", flush=True)
            return

        # Old firmware can run from here, so keep the retries back to back.
        print("Erase in reset failed; releasing reset and retrying", flush=True)
        ad3.hold_nrf_reset(False)
        for _ in range(retries):
            if _try_erase(dev_id):
                print("Erased after releasing reset", flush=True)
                return
        print("Erase failed: the nRF is not powered (nPZ2100 not in standby?)", flush=True)
        sys.exit(1)
    finally:
        ad3.hold_nrf_reset(False)
        if own:
            ad3.close()


class SerialCapture:
    """Reads log lines from a serial port in the background.

    Opened before flashing, so the boot that follows the flash is not
    missed. The nPZ2100 power-cycles the nRF on every wake, so the port
    keeps receiving lines from later wake cycles too.
    """

    spy = None  # an Ad3LogicCapture sniffing I2C next to the serial log

    def __init__(self, port, baudrate=BAUDRATE, spy=None):
        import serial  # pyserial; imported here so builds work without it

        self.lines = []
        self.spy = spy
        self._stop = threading.Event()
        self._ser = serial.Serial(port=port, baudrate=baudrate, timeout=0.1)
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        while not self._stop.is_set():
            raw = self._ser.readline()
            if raw:
                line = _ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
                _echo(line)
                self.lines.append(line)

    @property
    def i2c(self):
        """The I2C transactions sniffed during the capture, if any."""
        return self.spy.i2c if self.spy else []

    def wait(self, seconds):
        time.sleep(seconds)

    def close(self):
        self._stop.set()
        self._thread.join()
        self._ser.close()
        if self.spy:
            self.spy.close()


class Ad3UartCapture(SerialCapture):
    """Reads log lines from the nRF UART TX on an AD3 DIO pin.

    Used when the nRF is powered from SW_HP instead of USB, so there is no
    J-Link CDC port. Bytes garbled while the nRF is unpowered are kept as
    replacement characters; the line-based checks are not affected.
    """

    def __init__(self, pin, baudrate=BAUDRATE, ad3=None):
        self.lines = []
        self._stop = threading.Event()
        self._own = ad3 is None
        self._ad3 = open_ad3() if self._own else ad3
        self._ad3.uart_rx_setup(pin, baudrate)
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        pending = b""
        while not self._stop.is_set():
            pending += self._ad3.uart_read()
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                self._add(raw)
            time.sleep(0.01)
        if pending:
            self._add(pending)

    def _add(self, raw):
        line = _ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
        _echo(line)
        self.lines.append(line)

    def close(self):
        self._stop.set()
        self._thread.join()
        if self._own:
            self._ad3.close()  # without a device reset, so the supply stays on


def parse_i2c_pins(text):
    """'SDA,SCL' AD3 DIO pins (e.g. '6,7') -> (sda, scl). DIO0-2 are driven
    as outputs by the AD3 handler and the reset DIO is an output too, so
    they cannot be used."""
    import os

    try:
        sda, scl = (int(p.strip().lower().removeprefix("dio")) for p in text.split(","))
    except ValueError:
        raise ValueError(f"expected SDA,SCL AD3 DIO pins, e.g. 6,7, not '{text}'") from None
    reset_dio = int(os.environ.get("NPZ_CI_AD3_RESET_DIO") or 5)
    for pin in (sda, scl):
        if not 0 <= pin <= 15 or pin in (0, 1, 2, reset_dio):
            raise ValueError(f"AD3 DIO{pin} cannot sniff I2C (DIO0-2 and DIO{reset_dio} "
                             f"are outputs on this bench)")
    if sda == scl:
        raise ValueError("SDA and SCL must be different AD3 DIO pins")
    return sda, scl


def open_capture(port, baudrate=BAUDRATE, ad3=None, i2c=None):
    """Opens the log capture for port: a serial port, or ad3:<DIO pin>
    (e.g. ad3:4) for the AD3 UART receiver, on ad3 if one is open.

    With i2c=(SDA pin, SCL pin), the I2C bus is sniffed on those AD3 DIO
    pins too (ad3 must be open). The AD3 logic analyzer then records the
    bus, and the UART pin with it when the log comes from the AD3, and the
    capture's i2c lists the transactions."""
    uart_pin = None
    if port.lower().startswith("ad3:"):
        uart_pin = int(port[4:].lower().removeprefix("dio"))
    if i2c:
        from ad3_logic import Ad3LogicCapture, load_reg_names  # numpy, from dwfpy

        spy = Ad3LogicCapture(ad3, *i2c, uart_pin=uart_pin, baudrate=baudrate,
                              reg_names=load_reg_names(REPO_ROOT))
        if uart_pin is not None:
            return spy
        try:
            return SerialCapture(port, baudrate, spy=spy)
        except Exception:
            spy.close()
            raise
    if uart_pin is not None:
        print(f"Capturing the log on AD3 DIO{uart_pin}", flush=True)
        return Ad3UartCapture(uart_pin, baudrate, ad3)
    return SerialCapture(port, baudrate)


class Log:
    """Captured log with simple pattern checks that collect failures.

    i2c holds the sniffed I2C transactions (ad3_logic.I2cTransaction) when
    the run sniffs the bus, so a test can check register accesses too."""

    def __init__(self, lines, i2c=()):
        self.lines = lines
        self.i2c = list(i2c)
        self.failures = []

    def find(self, pattern):
        """All regex matches of pattern, one per matching line."""
        rx = re.compile(pattern)
        return [m for m in (rx.search(line) for line in self.lines) if m]

    def expect(self, pattern):
        if not self.find(pattern):
            self.failures.append(f"expected '{pattern}' in log")

    def expect_in_order(self, *patterns):
        pos = 0
        for pattern in patterns:
            rx = re.compile(pattern)
            while pos < len(self.lines) and not rx.search(self.lines[pos]):
                pos += 1
            if pos == len(self.lines):
                self.failures.append(f"expected '{pattern}' (in order) in log")
                return
            pos += 1

    def reject(self, pattern):
        for m in self.find(pattern):
            self.failures.append(f"unexpected '{m.string}'")


def common_checks(log):
    """Failure signatures every nPZ2100 sample shares."""
    log.reject(r"not ready")
    log.reject(r"failed: -\d+")  # driver calls return a negative errno
