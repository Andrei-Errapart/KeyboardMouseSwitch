"""Binary records the bench firmware streams over CDC, one per SLIP frame.

Little-endian; byte 0 is the type. Layouts follow the record table in
docs/superpowers/plans/2026-11-07-radio-bench-spike.md.
"""
import dataclasses
import struct
from dataclasses import dataclass
from typing import Union

KIND_HELLO, KIND_WELCOME, KIND_REPORT, KIND_PING, KIND_STATUS, KIND_POLL = (
    1, 2, 3, 4, 5, 6)
LINK_HELLO_HEARD, LINK_ESTABLISHED, LINK_LOST, LINK_ACTIVE, LINK_INACTIVE = (
    2, 3, 4, 5, 6)
ROLE_NONE, ROLE_BRIDGE, ROLE_RECEIVER = 0, 1, 2


@dataclass(frozen=True)
class Boot:
    t: int
    boot_id: int
    role: int
    cand: str
    serial: str
    fw: str


@dataclass(frozen=True)
class Text:
    text: str


@dataclass(frozen=True)
class Stats:
    t: int
    addr_tx: int
    addr_rx: int
    rec_dropped: int
    stamp_lost: int
    mic_fail: int
    replay_reject: int
    merged: int
    queue_dropped: int
    mode_switches: int
    mode_switch_us_total: int
    mode_switch_us_max: int


@dataclass(frozen=True)
class Queued:
    t: int
    id: int
    pipe: int


@dataclass(frozen=True)
class TxDone:
    t: int
    id: int
    pipe: int
    kind: int
    attempts: int
    ok: int
    len: int
    ack_len: int
    t_tx_addr: int
    t_rx_addr: int


@dataclass(frozen=True)
class RxPkt:
    t: int
    id: int
    pipe: int
    kind: int
    len: int
    rssi: int
    t_rx_addr: int


@dataclass(frozen=True)
class Link:
    t: int
    event: int
    pipe: int
    peer: bytes


@dataclass(frozen=True)
class Scan:
    channel: int
    rssi_min: int
    rssi_median: int
    rssi_max: int
    busy_permille: int


@dataclass(frozen=True)
class SelfTest:
    nist_ok: int
    layout_ok: int
    replay_ok: int
    tamper_ok: int
    enc_us: int
    dec_us: int
    sk_us: int


@dataclass(frozen=True)
class Merged:
    t: int
    id: int
    into: int
    pipe: int


Record = Union[Boot, Text, Stats, Queued, TxDone, RxPkt, Link, Scan, SelfTest,
               Merged]


def _boot(t, boot_id, role, cand, serial, fw):
    def text(raw):
        return raw.split(b"\0", 1)[0].decode("ascii", "replace")
    return Boot(t, boot_id, role, chr(cand) if cand else "", text(serial),
                text(fw))


_LAYOUTS = {
    0x01: (struct.Struct("<IIBB12s16s"), _boot),
    0x03: (struct.Struct("<11IH"), Stats),
    0x10: (struct.Struct("<IIB"), Queued),
    0x11: (struct.Struct("<IIBBBBBBII"), TxDone),
    0x12: (struct.Struct("<IIBBBbI"), RxPkt),
    0x13: (struct.Struct("<IBB6s"), Link),
    0x14: (struct.Struct("<BbbbH"), Scan),
    0x15: (struct.Struct("<BBBBHHH"), SelfTest),
    0x16: (struct.Struct("<IIIB"), Merged),
}
TYPE_TEXT = 0x02


def parse(frame: bytes) -> Record:
    if not frame:
        raise ValueError("empty frame")
    rtype, body = frame[0], frame[1:]
    if rtype == TYPE_TEXT:
        return Text(body.decode("utf-8", "replace"))
    if rtype not in _LAYOUTS:
        raise ValueError(f"unknown record type {rtype:#04x}")
    layout, make = _LAYOUTS[rtype]
    if len(body) != layout.size:
        raise ValueError(f"record type {rtype:#04x}: {len(body)} bytes, "
                         f"expected {layout.size}")
    return make(*layout.unpack(body))


class Unwrapper:
    """Turns the firmware's 32-bit microsecond times into a growing count.

    Each value is placed in the 2**32 window closest to the largest value
    seen so far, so stamps a little older than the record's own time land on
    the right side of a wrap. Valid while successive records are less than
    2**31 µs (35 min) apart.
    """
    WRAP = 1 << 32

    def __init__(self):
        self._max = None

    def reset(self):
        self._max = None

    def unwrap(self, t: int) -> int:
        if self._max is None:
            value = t
        else:
            near = self._max - self._max % self.WRAP + t
            value = min((c for c in (near - self.WRAP, near, near + self.WRAP)
                         if c >= 0), key=lambda c: abs(c - self._max))
        self._max = value if self._max is None else max(self._max, value)
        return value


_TIME_FIELDS = ("t", "t_tx_addr", "t_rx_addr")


def unwrap_all(records: list) -> list:
    """Unwrap the times of one dongle's records, restarting at each Boot.
    A stamp of 0 means unknown and stays 0."""
    u, out = Unwrapper(), []
    for rec in records:
        if isinstance(rec, Boot):
            u.reset()
        fields = {f: u.unwrap(getattr(rec, f)) if f == "t" or getattr(rec, f) else 0
                  for f in _TIME_FIELDS if hasattr(rec, f)}
        out.append(dataclasses.replace(rec, **fields) if fields else rec)
    return out
