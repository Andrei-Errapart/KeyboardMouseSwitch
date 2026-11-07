"""Bench runs: drive both dongles, collect both record streams, analyse."""
import threading
import time

from . import clockfit, metrics, port
from .records import Link, LINK_ESTABLISHED, Text, unwrap_all

BRIDGE, RECEIVER = "anduq7", "macmini"
# A learns it is active only at an idle poll (up to ~200 ms, see README):
# latency runs start after that; T5 measures the switch itself.
ACTIVATE_SETTLE_S = 0.5


class Recorder:
    """Reads one dongle's records in a background thread."""

    def __init__(self, name, opener=port.open_port):
        self.name = name
        self.port = opener(name)
        self.records = []        # (host monotonic time, record)
        self.error = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        try:
            while not self._stop.is_set():
                for rec in self.port.records(timeout=0.2):
                    self.records.append((time.monotonic(), rec))
        except Exception as e:   # PortClosed / PortStalled end the run
            self.error = e

    def send(self, cmd):
        self.port.send(cmd)

    def mark(self):
        return len(self.records)

    def wait_for(self, pred, timeout, since=0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for _, rec in self.records[since:]:
                if pred(rec):
                    return rec
            if self.error:
                raise self.error
            time.sleep(0.05)
        return None

    def texts(self, since=0):
        return [r.text for _, r in self.records[since:] if isinstance(r, Text)]

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=3)
        self.port.close()
        if self.error:
            raise self.error
        return [r for _, r in self.records]


def reset(*recorders, settle=1.5):
    """Stop any role and let records from earlier runs drain, so a run
    never reads another run's backlog. Returns the marks to read from."""
    for x in recorders:
        x.send("kmb role none")
    time.sleep(settle)
    return [x.mark() for x in recorders]


def start_link(b, r, cand, timeout=3.0, settings=()):
    """Bridge and receiver roles for `cand`, after `kmb set` lines given in
    `settings` (sent to both); returns the receiver's pipe."""
    since_b, since_r = reset(b, r)
    for line in settings:
        b.send(line)
        r.send(line)
    b.send(f"kmb role bridge {cand}")
    r.send(f"kmb role receiver {cand}")
    est = lambda rec: isinstance(rec, Link) and rec.event == LINK_ESTABLISHED
    link = b.wait_for(est, timeout, since_b)
    if link is None or r.wait_for(est, 1.0, since_r) is None:
        raise RuntimeError(f"no link within {timeout} s: bridge {b.texts(since_b)} "
                           f"receiver {r.texts(since_r)}")
    return link.pipe


def finish(b, r):
    """End a run: stop both roles, so pings and refreshes stop too, then
    take fresh STATS from both (see metrics.check_run)."""
    for x in (b, r):
        x.send("kmb role none")
    time.sleep(0.3)
    for x in (b, r):
        x.send("kmb stats")
    time.sleep(1.5)
    return unwrap_all(b.stop()), unwrap_all(r.stop())


def analyse(bridge, receiver):
    for recs in (bridge, receiver):
        metrics.check_run(recs)
    fit = clockfit.fit(clockfit.pairs(bridge, receiver))
    return fit, metrics.latency(bridge, receiver, fit)


def smoke(cand, ch=None):
    b, r = Recorder(BRIDGE), Recorder(RECEIVER)
    try:
        pipe = start_link(b, r, cand, settings=[f"kmb set ch {ch}"] if ch is not None else [])
        b.send(f"kmb active {pipe}")
        time.sleep(ACTIVATE_SETTLE_S)
        b.send("kmb traffic single 200 20 50")
        time.sleep(15)
    except Exception:
        b.stop(), r.stop()
        raise
    bridge, receiver = finish(b, r)
    fit, lat = analyse(bridge, receiver)
    print(f"smoke {cand}{f' ch {ch}' if ch is not None else ''}: delivered {lat.delivered}, lost {lat.lost}, dups {lat.dups}; "
          f"clock fit n={fit.n} resid p99 {fit.resid_p99_us:.1f} µs, "
          f"drift {fit.drift_ppm:.1f} ppm")
    print(f"latency µs: p50 {lat.p50:.0f}  p99 {lat.p99:.0f}  max {lat.max:.0f}")
    return fit, lat
