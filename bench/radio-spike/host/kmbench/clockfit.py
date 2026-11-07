"""Fit the receiver's clock to the bridge's from shared ADDRESS events.

A packet that got through on its first try was stamped by both dongles at
the same instant: the sender's TX ADDRESS event and the receiver's RX
ADDRESS event. Those pairs fix the offset and drift between the clocks.
"""
import math
from dataclasses import dataclass

from .records import RxPkt, TxDone

MIN_PAIRS = 20
MAX_RESID_P99_US = 20.0


class ClockFitError(Exception):
    pass


def percentile(values, p: float) -> float:
    """Nearest-rank percentile; NaN for no values."""
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100 * len(ordered)))
    return float(ordered[rank - 1])


@dataclass(frozen=True)
class Fit:
    offset_us: float
    drift_ppm: float
    resid_p99_us: float
    n: int

    def to_bridge(self, t_receiver: int) -> float:
        return (t_receiver - self.offset_us) / (1 + self.drift_ppm / 1e6)


def _first_try(senders, receivers):
    """(TxDone, RxPkt) for packets the receiver got on the first attempt."""
    received = {}
    for rec in receivers:
        if isinstance(rec, RxPkt):
            received.setdefault((rec.kind, rec.id), rec)
    for rec in senders:
        if isinstance(rec, TxDone) and rec.attempts == 1 and rec.ok:
            rx = received.get((rec.kind, rec.id))
            if rx is not None:
                yield rec, rx


def pairs(bridge: list, receiver: list) -> list:
    """(t_bridge, t_receiver) for every packet both clocks stamped. A stamp
    of 0 means the firmware couldn't attribute it, and is skipped."""
    out = [(tx.t_tx_addr, rx.t_rx_addr) for tx, rx in _first_try(bridge, receiver)]
    out += [(rx.t_rx_addr, tx.t_tx_addr) for tx, rx in _first_try(receiver, bridge)]
    return [(b, r) for b, r in out if b and r]


def fit(pairs: list) -> Fit:
    n = len(pairs)
    if n < MIN_PAIRS:
        raise ClockFitError(f"only {n} clock pairs, need {MIN_PAIRS}")
    mx = sum(x for x, _ in pairs) / n
    my = sum(y for _, y in pairs) / n
    sxx = sum((x - mx) ** 2 for x, _ in pairs)
    sxy = sum((x - mx) * (y - my) for x, y in pairs)
    slope = sxy / sxx if sxx else 1.0
    offset = my - slope * mx
    resid = [abs(y - (slope * x + offset)) for x, y in pairs]
    p99 = percentile(resid, 99)
    if p99 > MAX_RESID_P99_US:
        raise ClockFitError(f"clock fit residual p99 {p99:.1f} µs exceeds "
                            f"{MAX_RESID_P99_US} µs")
    return Fit(offset_us=offset, drift_ppm=(slope - 1) * 1e6,
               resid_p99_us=p99, n=n)
