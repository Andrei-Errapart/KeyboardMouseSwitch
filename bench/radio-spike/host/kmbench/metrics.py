"""Metrics for one bench run, from both dongles' unwrapped records."""
import math
from dataclasses import dataclass

from .clockfit import Fit, percentile
from .records import (KIND_REPORT, LINK_ACTIVE, LINK_ESTABLISHED, Boot, Link,
                      Merged, Queued, RxPkt, Stats, Text, TxDone)

FAST_RAMP_UP_US = 40


class RunInvalid(Exception):
    pass


@dataclass(frozen=True)
class Latency:
    samples_us: list
    p50: float
    p99: float
    max: float
    delivered: int
    lost: int
    dups: int
    merged: int


def air(n: int) -> float:
    """On-air µs of an ESB packet with an n-byte payload at 2 Mbit/s:
    2-byte preamble, 5-byte address, 9-bit PCF, 16-bit CRC."""
    return 4 * (2 + 5 + n + 2) + 4.5


def latency(bridge: list, receiver: list, fit: Fit) -> Latency:
    """Bridge queue time to the receiver application, per test report."""
    into = {m.id: m.into for m in bridge if isinstance(m, Merged)}
    first, dups = {}, 0
    for rec in receiver:
        if isinstance(rec, RxPkt) and rec.kind == KIND_REPORT:
            if rec.id in first:
                dups += 1
            else:
                first[rec.id] = rec
    samples, delivered, lost, merged = [], 0, 0, 0
    for q in (r for r in bridge if isinstance(r, Queued)):
        target, seen = q.id, {q.id}
        while target in into and into[target] not in seen:
            target = into[target]
            seen.add(target)
        if target != q.id:
            merged += 1
        got = first.get(target)
        if got is None:
            lost += 1
        else:
            delivered += 1
            samples.append(fit.to_bridge(got.t) - q.t)
    return Latency(samples_us=samples, p50=percentile(samples, 50),
                   p99=percentile(samples, 99),
                   max=max(samples) if samples else math.nan,
                   delivered=delivered, lost=lost, dups=dups, merged=merged)


def lost_after_ack(bridge: list, receiver: list) -> list:
    """Ids of lost reports the bridge saw acknowledged on air (B). ESB's
    receiver drops a packet whose PID and CRC match the previous one as a
    retransmission, after acknowledging it; the bench bridge restarts PIDs
    at every re-initialisation of ESB. Not radio loss, and no retry helps."""
    into = {m.id: m.into for m in bridge if isinstance(m, Merged)}
    got = {r.id for r in receiver if isinstance(r, RxPkt) and r.kind == KIND_REPORT}
    acked = {t.id for t in bridge if isinstance(t, TxDone) and t.kind == KIND_REPORT and t.ok}
    out = []
    for q in (r for r in bridge if isinstance(r, Queued)):
        target, seen = q.id, {q.id}
        while target in into and into[target] not in seen:
            target = into[target]
            seen.add(target)
        if target not in got and target in acked:
            out.append(q.id)
    return out


def airtime(records: list, seconds: float) -> float:
    """Fraction of time a packet of ours is on air."""
    busy = sum(r.attempts * air(r.len) + (air(r.ack_len) if r.ok else 0)
               for r in records if isinstance(r, TxDone))
    return busy / (seconds * 1e6)


def transaction_us(records: list) -> list:
    """Length of each first-try transaction, ramp-up to the end of the ACK."""
    return [FAST_RAMP_UP_US + (r.t_rx_addr - r.t_tx_addr) + air(r.ack_len)
            for r in records
            if isinstance(r, TxDone) and r.attempts == 1 and r.ok]


def switch_delays(bridge: list, receiver: list, fit: Fit) -> list:
    """ms from each activation to the first report the receiver gets."""
    acts = sorted(r.t for r in bridge
                  if isinstance(r, Link) and r.event == LINK_ACTIVE)
    reports = sorted(fit.to_bridge(r.t) for r in receiver
                     if isinstance(r, RxPkt) and r.kind == KIND_REPORT)
    out = []
    for i, start in enumerate(acts):
        end = acts[i + 1] if i + 1 < len(acts) else math.inf
        got = next((t for t in reports if start < t < end), None)
        if got is not None:
            out.append((got - start) / 1000)
    return out


def discovery_times(records: list) -> list:
    """ms from each boot to the link being established, on one dongle."""
    out, booted = [], None
    for rec in records:
        if isinstance(rec, Boot):
            booted = rec.t
        elif (isinstance(rec, Link) and rec.event == LINK_ESTABLISHED
              and booted is not None):
            out.append((rec.t - booted) / 1000)
            booted = None
    return out


# stamp_lost is not here: lost stamps are zeroed and skipped by the clock
# fit, so they cost pairs, not correctness.
_COUNTERS = ("rec_dropped",)


def check_run(records: list, allow_reboot: bool = False) -> None:
    """Raise RunInvalid if the records can't be trusted."""
    if not allow_reboot and any(isinstance(r, Boot) for r in records[1:]):
        raise RunInvalid("the dongle rebooted during the run")
    last_other = max((i for i, r in enumerate(records)
                      if not isinstance(r, (Stats, Text))), default=-1)
    if not any(isinstance(r, Stats) for r in records[last_other + 1:]):
        raise RunInvalid("no STATS after the run's last record; end every run "
                         "with `kmb stats`")
    segments = [[]]
    for rec in records:
        if isinstance(rec, Boot):
            segments.append([])
        elif isinstance(rec, Stats):
            segments[-1].append(rec)
    for stats in segments:
        if len(stats) < 2:
            continue
        for name in _COUNTERS:
            rise = getattr(stats[-1], name) - getattr(stats[0], name)
            if rise > 0:
                raise RunInvalid(f"{name} rose by {rise} during the run")
