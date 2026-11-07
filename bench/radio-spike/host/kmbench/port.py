"""Reaching the bench dongles: the Mac mini's directly, anduq7's over SSH.

A Transport moves bytes; a Port turns them into records and sends `kmb`
commands. The flash helpers live here too, because flashing is mostly
waiting for ports to disappear and come back.
"""
import collections
import glob
import os
import select
import subprocess
import termios
import time
import tty
from dataclasses import dataclass

from . import records, slip

STALL_S = 2.0
CLOSE_WAIT_S = 3.0
REPO_ON_ANDUQ7 = "/data/workspace/macmini/KeyboardMouseSwitch"
REMOTE_NRFUTIL = "~/.local/bin/nrfutil"
DFU_PING = bytes([0x09, 0x5A])
DFU_PONG = bytes([0x60, 0x09, 0x01, 0x5A])


class PortClosed(Exception):
    pass


class PortStalled(Exception):
    pass


@dataclass(frozen=True)
class Dongle:
    name: str
    serial: str
    host: str | None      # None: attached to this computer
    port_glob: str        # matches the bootloader's port and ours


DONGLES = {
    "macmini": Dongle("macmini", "E1380B5925A4", None,
                      "/dev/cu.usbmodemE1380B5925A4*"),
    "anduq7": Dongle("anduq7", "DA1EAB2D90C1", "anduq7",
                     "/dev/serial/by-id/usb-*_DA1EAB2D90C1-if00"),
}


def remote_command(d: Dongle) -> list:
    return ["ssh", "-T", "-o", "ServerAliveInterval=1", "-o",
            "ServerAliveCountMax=2", "-o", "BatchMode=yes", d.host,
            f'p=$(ls {d.port_glob}) && exec socat - "$p",raw,echo=0']


class Transport:
    def __init__(self, rfd, wfd, proc=None):
        self.rfd, self.wfd, self.proc = rfd, wfd, proc

    @classmethod
    def from_command(cls, argv):
        proc = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, bufsize=0)
        return cls(proc.stdout.fileno(), proc.stdin.fileno(), proc)

    @classmethod
    def from_device(cls, path):
        # No input flush, and TCSANOW rather than setraw's default
        # TCSAFLUSH: opening raises DTR, and the dongle answers at once with
        # the records it held back, such as the Boot record.
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd, termios.TCSANOW)
        return cls(fd, fd)

    def read(self, timeout: float) -> bytes:
        """Bytes available within timeout (maybe none); PortClosed on EOF."""
        try:
            ready, _, _ = select.select([self.rfd], [], [], max(0, timeout))
            if not ready:
                return b""
            data = os.read(self.rfd, 4096)
        except BlockingIOError:
            return b""
        except OSError as e:
            raise PortClosed(str(e)) from e
        if not data:
            raise PortClosed("end of file")
        return data

    def write(self, data: bytes):
        view = memoryview(data)
        try:
            while view:
                try:
                    view = view[os.write(self.wfd, view):]
                except BlockingIOError:
                    select.select([], [self.wfd], [], 0.1)
        except OSError as e:
            raise PortClosed(str(e)) from e

    def close(self):
        """Free the port. A command (ssh + socat) gets end of input and is
        waited for: killing ssh would leave the remote socat holding the
        serial port for a moment, and a DFU started then fails."""
        if self.proc is None:
            try:
                os.close(self.rfd)
            except OSError:
                pass
            return
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=CLOSE_WAIT_S)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()


class Port:
    """Records from one dongle, and `kmb` commands to it."""

    def __init__(self, transport: Transport, stall_s: float = STALL_S):
        self.transport = transport
        self.stall_s = stall_s
        self.bad_frames = 0
        self._decoder = slip.Decoder()
        self._pending = collections.deque()
        self._last_stats = time.monotonic()

    def send(self, cmd: str):
        self.transport.write((cmd + "\n").encode())

    def _pump(self, timeout: float):
        for frame in self._decoder.feed(self.transport.read(timeout)):
            try:
                self._pending.append(records.parse(frame))
            except ValueError:
                self.bad_frames += 1

    def first(self, timeout: float):
        """The next record, left in place for records(); None on timeout."""
        end = time.monotonic() + timeout
        while not self._pending:
            left = end - time.monotonic()
            if left <= 0:
                return None
            self._pump(min(0.1, left))
        return self._pending[0]

    def records(self, timeout: float):
        """Yield records for `timeout` seconds. PortClosed at end of file,
        PortStalled after STALL_S without a STATS record."""
        end = time.monotonic() + timeout
        while True:
            while self._pending:
                rec = self._pending.popleft()
                if isinstance(rec, records.Stats):
                    self._last_stats = time.monotonic()
                yield rec
            now = time.monotonic()
            if now >= end:
                return
            if now - self._last_stats > self.stall_s:
                raise PortStalled(f"no STATS for {self.stall_s} s")
            self._pump(min(0.1, end - now))

    def close(self):
        self.transport.close()


def port_path(name: str) -> str:
    d = DONGLES[name]
    if d.host is None:
        found = sorted(glob.glob(d.port_glob))
        if not found:
            raise FileNotFoundError(f"{name}: no port matches {d.port_glob}")
        return found[0]
    out = subprocess.run(["ssh", "-o", "BatchMode=yes", d.host,
                          f"ls {d.port_glob}"], capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        raise FileNotFoundError(f"{name}: no port matches {d.port_glob}")
    return out.stdout.split()[0]


def open_transport(name: str) -> Transport:
    d = DONGLES[name]
    if d.host is None:
        return Transport.from_device(port_path(name))
    return Transport.from_command(remote_command(d))


def open_port(name: str) -> Port:
    return Port(open_transport(name))


def reopen(name: str, timeout: float = 10, opener=open_transport) -> Port:
    """Open a port that is coming back after a reboot the run asked for.
    The port counts as back once it delivers a record."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            p = Port(opener(name))
            if p.first(min(2.5, max(0.1, deadline - time.monotonic()))):
                return p
            p.close()
        except (PortClosed, OSError):
            pass
        if time.monotonic() >= deadline:
            raise PortClosed(f"{name}: port not back within {timeout} s")
        time.sleep(0.3)


def dfu_ping(t: Transport, timeout: float = 1.0) -> bool:
    t.write(slip.encode(DFU_PING))
    decoder, end = slip.Decoder(), time.monotonic() + timeout
    while time.monotonic() < end:
        if DFU_PONG in decoder.feed(t.read(0.1)):
            return True
    return False


def wait_for_bootloader(name: str, timeout: float = 10,
                        opener=open_transport) -> str:
    """The bootloader's port, once it answers a DFU ping."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            t = opener(name)
            try:
                if dfu_ping(t):
                    return port_path(name)
            finally:
                t.close()
        except (PortClosed, OSError):
            pass
        time.sleep(0.3)
    raise TimeoutError(f"{name}: no DFU bootloader within {timeout} s")
