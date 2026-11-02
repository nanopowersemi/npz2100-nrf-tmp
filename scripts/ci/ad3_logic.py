"""Logic analyzer capture on the AD3: the nRF UART log and the I2C bus together.

The AD3 protocol engine (used by Ad3UartCapture) runs one protocol at a
time, so it cannot receive the UART and spy on I2C at once. Instead the
logic analyzer records the UART, SDA and SCL pins as one sample stream,
and both are decoded here in software. That also puts the log lines and
the I2C transactions on one time base, so each register access shows up
between the log lines it happened between.

Environment:
    NPZ_CI_AD3_LA_RATE    logic analyzer sample rate in Hz (default 2000000)
"""

import os
import re
import sys
import threading
import time
from collections import Counter
from pathlib import Path

import numpy as np

LA_RATE = float(os.environ.get("NPZ_CI_AD3_LA_RATE") or 2e6)
_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_REG_DEFINE = re.compile(r"#define\s+NPZ2100_REG_(\w+)\s+\((0x[0-9A-Fa-f]+)u?\)")


def load_reg_names(repo_root):
    """Register address -> name, from the NPZ2100_REG_* defines."""
    names = {}
    for header in sorted(Path(repo_root).glob("npz2100_module/include/npz2100_regs_*.h")):
        for m in _REG_DEFINE.finditer(header.read_text(errors="replace")):
            names.setdefault(int(m.group(2), 16), m.group(1))
    return names


def _print(text):
    # Garbled bytes become U+FFFD, which a non-UTF-8 console cannot print.
    enc = sys.stdout.encoding or "utf-8"
    print(text.encode(enc, errors="replace").decode(enc), flush=True)


class UartDecoder:
    """8N1 UART receiver working on a stream of 0/1 samples."""

    def __init__(self, sample_rate, baudrate):
        self.spb = sample_rate / baudrate  # samples per bit
        self._buf = np.ones(0, dtype=np.uint8)
        self._base = 0  # absolute sample index of _buf[0]
        self.framing_errors = 0

    def reset(self, start):
        """Drops any partial byte, e.g. after lost samples."""
        self._buf = np.ones(0, dtype=np.uint8)
        self._base = start

    def feed(self, bits, start):
        """Feeds samples start.. and returns [(sample index, byte)]."""
        if self._buf.size == 0:
            self._base = start
        buf = np.concatenate([self._buf, bits])
        spb = self.spb
        frame = int(9.5 * spb) + 1
        out = []
        pos = 0
        keep = len(buf) - 1  # always keep the last sample, to see the next edge
        for f in np.flatnonzero((buf[:-1] == 1) & (buf[1:] == 0)) + 1:
            if f < pos:
                continue
            if f + frame >= len(buf):
                keep = f - 1  # frame not complete yet: decode it next time
                break
            if buf[f + int(0.5 * spb)]:
                continue  # glitch, not a start bit
            byte = 0
            for i in range(8):
                byte |= int(buf[f + int((1.5 + i) * spb)]) << i
            if buf[f + int(9.5 * spb)]:
                out.append((self._base + f, byte))
            else:
                self.framing_errors += 1  # e.g. noise while the nRF is unpowered
            pos = f + int(9.5 * spb)
        keep = max(keep, 0)
        self._buf = buf[keep:]
        self._base += keep
        return out


class I2cSegment:
    """One address byte and the data after it, up to the next (repeated) start."""

    def __init__(self, addr_byte):
        self.addr = addr_byte >> 1
        self.read = bool(addr_byte & 1)
        self.data = bytearray()
        self.nak = False  # the address or a written byte was not acknowledged

    def __str__(self):
        text = f"{'R' if self.read else 'W'}"
        if self.data:
            text += " " + " ".join(f"{b:02X}" for b in self.data)
        return text + (" NAK" if self.nak else "")


class I2cTransaction:
    """Everything from a START to the STOP, repeated starts included."""

    def __init__(self, sample):
        self.sample = sample
        self.time = 0.0  # seconds since the capture started, set when complete
        self.segments = []

    @property
    def addr(self):
        return self.segments[0].addr if self.segments else None

    @property
    def nak(self):
        return any(s.nak for s in self.segments)

    def register_access(self):
        """('write', reg, data) or ('read', reg, data) for the usual
        register protocol (1-byte register address, auto-increment), else None."""
        s = self.segments
        if len(s) == 1 and not s[0].read and len(s[0].data) >= 2 and not s[0].nak:
            return "write", s[0].data[0], bytes(s[0].data[1:])
        if (len(s) == 2 and not s[0].read and len(s[0].data) == 1 and s[1].read
                and not self.nak):
            return "read", s[0].data[0], bytes(s[1].data)
        return None

    def raw(self):
        return f"{self.addr:02X}: " + " | ".join(str(s) for s in self.segments)


class I2cDecoder:
    """I2C bus monitor working on a stream of (SCL << 1 | SDA) samples."""

    def __init__(self):
        self._prev = 3  # bus idle: both lines high
        self._txn = None
        self._seg = None
        self._bits = 0
        self._count = 0

    def reset(self):
        """Drops a partial transaction, e.g. after lost samples."""
        self._txn = None
        self._prev = 3

    def feed(self, v, start):
        """Feeds samples start.. and returns the transactions that ended."""
        out = []
        prev = np.empty_like(v)
        prev[0] = self._prev
        prev[1:] = v[:-1]
        for i in np.flatnonzero(v != prev):
            old, new = int(prev[i]), int(v[i])
            if old & new & 2:  # SCL high throughout: START or STOP
                if old & 1 and not new & 1:
                    self._start(start + i)
                elif not old & 1 and new & 1 and self._txn is not None:
                    if self._txn.segments:
                        out.append(self._txn)
                    self._txn = None
            elif new & 2 and not old & 2:  # SCL rising: sample SDA
                self._bit(new & 1)
        self._prev = int(v[-1])
        return out

    def _start(self, sample):
        if self._txn is None:
            self._txn = I2cTransaction(sample)
        self._seg = None  # a repeated start begins a new segment
        self._bits = self._count = 0

    def _bit(self, bit):
        if self._txn is None:
            return  # no START seen (capture began mid-transfer)
        if self._count < 8:
            self._bits = (self._bits << 1) | bit
            self._count += 1
            if self._count == 8:
                if self._seg is None:
                    self._seg = I2cSegment(self._bits)
                    self._txn.segments.append(self._seg)
                else:
                    self._seg.data.append(self._bits)
            return
        # 9th bit: ACK (low) or NAK (high). A NAK on the last byte of a
        # read is normal (the controller ends the read with it).
        if bit and not self._seg.read:
            self._seg.nak = True
        if bit and self._seg.read and not self._seg.data:
            self._seg.nak = True  # address NAK on a read
        self._bits = self._count = 0


class Ad3LogicCapture:
    """Records SDA/SCL (and optionally the nRF UART TX) with the AD3 logic
    analyzer in the background, and decodes both as the samples come in.

    lines holds the UART log lines (as the other captures do), i2c the
    completed I2cTransactions. The UART pin is optional, so I2C can also be
    sniffed while the log comes from a serial port.
    """

    def __init__(self, ad3, sda, scl, uart_pin=None, baudrate=115200, rate=LA_RATE,
                 reg_names=None, max_s=3600):
        import dwfpy as dwf

        pins = [p for p in (sda, scl, uart_pin) if p is not None]
        if len(set(pins)) != len(pins):
            raise ValueError(f"AD3 pins must differ: SDA {sda}, SCL {scl}, UART {uart_pin}")
        self.lines = []
        self.i2c = []
        self.lost = 0
        self._sda, self._scl, self._uart_pin = sda, scl, uart_pin
        self._regs = reg_names or {}
        self._uart = UartDecoder(rate, baudrate) if uart_pin is not None else None
        self._i2c = I2cDecoder()
        self._pending = b""
        self._pending_sample = None
        self._n = 0  # samples received so far
        self._stop = threading.Event()

        self._di = ad3.device.digital_input
        di = self._di
        di.reset()
        di.acquisition_mode = dwf.AcquisitionMode.RECORD
        di.sample_format = 8 if max(pins) < 8 else 16
        di.sample_rate = rate
        self.rate = di.sample_rate  # the rate the divider actually gives
        di.setup_trigger(source="none", position=min(int(self.rate * max_s), 2**31 - 1), prefill=0)
        di.configure(start=True)

        what = f"I2C on DIO{sda} (SDA) / DIO{scl} (SCL)"
        if uart_pin is not None:
            what = f"the log on DIO{uart_pin} and " + what
        print(f"AD3 logic analyzer: capturing {what} at {self.rate / 1e6:g} MS/s", flush=True)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        try:
            while not self._stop.is_set():
                self._poll()
                time.sleep(0.005)
            self._poll()
        except Exception as e:  # keep the test going; the failure shows in the summary
            print(f"AD3 logic analyzer stopped: {e}", flush=True)

    def _poll(self):
        di = self._di
        di.read_status(read_data=True)
        available, lost, corrupted = di.record_status
        if lost or corrupted:
            self.lost += lost + corrupted
            self._n += lost
            print(f"AD3: logic analyzer lost {lost + corrupted} samples "
                  f"(lower NPZ_CI_AD3_LA_RATE?)", flush=True)
            self._i2c.reset()
            if self._uart:
                self._uart.reset(self._n)
        if not available:
            return
        data = di.get_data(sample_count=available)
        start, self._n = self._n, self._n + available

        events = []
        if self._uart:
            for sample, byte in self._uart.feed(((data >> self._uart_pin) & 1).astype(np.uint8), start):
                events += self._uart_byte(sample, byte)
        v = ((((data >> self._scl) & 1) << 1) | ((data >> self._sda) & 1)).astype(np.uint8)
        for txn in self._i2c.feed(v, start):
            txn.time = txn.sample / self.rate
            self.i2c.append(txn)
            events.append((txn.sample, "[I2C ] " + self.describe(txn)))
        for _, text in sorted(events, key=lambda e: e[0]):
            _print(text)

    def _uart_byte(self, sample, byte):
        if self._pending_sample is None:
            self._pending_sample = sample
        if byte != 0x0A:
            self._pending += bytes([byte])
            return []
        return [self._end_line()]

    def _end_line(self):
        sample = self._pending_sample
        line = _ANSI.sub("", self._pending.decode("utf-8", errors="replace")).rstrip()
        self._pending, self._pending_sample = b"", None
        self.lines.append(line)
        return sample, f"[UART] {line}"

    def _reg(self, reg):
        name = self._regs.get(reg)
        return f"{name} (0x{reg:02X})" if name else f"0x{reg:02X}"

    def describe(self, txn):
        access = txn.register_access()
        t = f"{txn.time:10.6f} s  "
        if access is None:
            return t + txn.raw()
        kind, reg, data = access
        values = " ".join(f"{b:02X}" for b in data)
        if kind == "write":
            return t + f"write {self._reg(reg)} = {values}"
        return t + f"read  {self._reg(reg)} -> {values}"

    def summary(self):
        """Register accesses per register, as printable lines."""
        writes, reads, other = Counter(), Counter(), 0
        for txn in self.i2c:
            access = txn.register_access()
            if access is None:
                other += 1
                continue
            kind, reg, data = access
            # Burst accesses count for every register they touch.
            for r in range(reg, reg + len(data)):
                (writes if kind == "write" else reads)[r & 0xFF] += 1
        naks = sum(1 for t in self.i2c if t.nak)
        out = [f"I2C: {len(self.i2c)} transactions, {sum(writes.values())} register writes, "
               f"{sum(reads.values())} register reads, {naks} NAK, {other} other"]
        if writes or reads:
            out.append(f"  {'register':<24} {'writes':>6} {'reads':>6}")
            for reg in sorted(set(writes) | set(reads)):
                out.append(f"  {self._reg(reg):<24} {writes[reg]:>6} {reads[reg]:>6}")
        if self.lost:
            out.append(f"  {self.lost} samples lost: some transactions may be missing")
        return out

    def wait(self, seconds):
        time.sleep(seconds)

    def close(self):
        self._stop.set()
        self._thread.join()
        self._di.reset()  # stop recording; the supply and reset pin are not touched
        if self._pending:
            _print(self._end_line()[1])
        for line in self.summary():
            print(line, flush=True)
