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


# --- Test runs T1–T8 (plan, Task 7) ---------------------------------------

import datetime
import json
import pathlib
import pickle
import random

from . import port as _port
from .records import Boot, LINK_LOST, RxPkt, Scan, Stats, TxDone, KIND_POLL, KIND_REPORT

RESULTS = pathlib.Path(__file__).resolve().parents[2] / "results"
SEVEN_EXTRA = 6           # receivers known besides the active one
IDLE_HZ = 10              # idle receivers are pinged (B) or poll (A) every 100 ms


def save(test, cand, ch, bridge, receiver, result):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    d = RESULTS / f"{stamp}-{test}-{cand}"
    d.mkdir(parents=True, exist_ok=True)
    for name, recs in (("bridge", bridge), ("receiver", receiver)):
        with open(d / f"{name}.pkl", "wb") as f:
            pickle.dump(recs, f)
    result = {"test": test, "cand": cand, "ch": ch, **result}
    (d / "result.json").write_text(json.dumps(result, indent=1, default=float))
    return d, result


def wait_text(rec, text, since, timeout):
    if rec.wait_for(lambda x: isinstance(x, Text) and x.text == text, timeout, since) is None:
        raise RuntimeError(f"no '{text}' within {timeout} s")


def counter_rise(recs, name):
    stats = [x for x in recs if isinstance(x, Stats)]
    return getattr(stats[-1], name) - getattr(stats[0], name) if len(stats) > 1 else 0


def latency_result(bridge, receiver):
    for recs in (bridge, receiver):
        metrics.check_run(recs)
    fit = clockfit.fit(clockfit.pairs(bridge, receiver))
    lat = metrics.latency(bridge, receiver, fit)
    return {"p50": lat.p50, "p99": lat.p99, "max": lat.max, "delivered": lat.delivered,
            "lost": lat.lost, "dups": lat.dups, "merged": counter_rise(bridge, "merged"),
            "queue_dropped": counter_rise(bridge, "queue_dropped"),
            "fit_n": fit.n, "resid_p99": fit.resid_p99_us,
            "mic_fail": counter_rise(bridge, "mic_fail") + counter_rise(receiver, "mic_fail"),
            "stamp_lost": counter_rise(bridge, "stamp_lost") + counter_rise(receiver, "stamp_lost")}


def linked(cand, ch, body, phantom=0, settings=()):
    """Link, activate the receiver, run body(b, r, pipe); returns records."""
    b, r = Recorder(BRIDGE), Recorder(RECEIVER)
    try:
        pipe = start_link(b, r, cand, settings=[f"kmb set ch {ch}", *settings])
        if phantom:
            b.send(f"kmb phantom {phantom}")
        b.send(f"kmb active {pipe}")
        time.sleep(ACTIVATE_SETTLE_S)
        extra = body(b, r, pipe) or {}
    except BaseException:
        for x in (b, r):
            try:
                x.stop()
            except Exception:
                pass
        raise
    bridge, receiver = finish(b, r)
    return bridge, receiver, extra


def traffic(b, cmd, timeout):
    since = b.mark()
    b.send(f"kmb traffic {cmd}")
    wait_text(b, "ok traffic done", since, timeout)
    time.sleep(0.5)  # let the last reports arrive


def run_t1(cand, ch, n=1000, phantom=0):
    bridge, receiver, _ = linked(cand, ch, lambda b, r, p: traffic(
        b, f"single {n} 20 200", n * 0.2 + 30), phantom=phantom)
    return bridge, receiver, latency_result(bridge, receiver)


def run_t2(cand, ch):
    def body(b, r, p):
        traffic(b, "burst 50 10 5 300", 60)
        traffic(b, "stream 1000 10", 40)
    bridge, receiver, _ = linked(cand, ch, body)
    return bridge, receiver, latency_result(bridge, receiver)


def run_t3(cand, ch, seconds=60):
    bridge, receiver, _ = linked(cand, ch, lambda b, r, p: time.sleep(seconds))
    for recs in (bridge, receiver):
        metrics.check_run(recs)
    tx = [x for x in bridge + receiver if isinstance(x, TxDone)]
    busy = metrics.airtime(tx, seconds)
    trans = metrics.transaction_us(tx)
    # Each further idle receiver: a 16-byte PING (B) or POLL (A) and a
    # 16-byte STATUS in its ACK, 10 times a second.
    idle = IDLE_HZ * 2 * metrics.air(16) / 1e6
    return bridge, receiver, {
        "busy_fraction": busy, "busy_fraction_seven": busy + SEVEN_EXTRA * idle,
        "transaction_us": clockfit.percentile(trans, 50), "transactions": len(tx)}


def run_t4(cand, ch, n=100_000, hz=200):
    bridge, receiver, _ = linked(cand, ch, lambda b, r, p: traffic(
        b, f"stream {hz} {n // hz}", n / hz + 60))
    res = latency_result(bridge, receiver)
    total = res["delivered"] + res["lost"]
    sender = bridge if cand == "B" else receiver
    kind = KIND_REPORT if cand == "B" else KIND_POLL
    att = [x.attempts for x in sender if isinstance(x, TxDone) and x.kind == kind]
    res["loss_per_10k"] = res["lost"] * 10_000 / total if total else float("nan")
    res["retries"] = {str(k): att.count(k) for k in sorted(set(att))}
    return bridge, receiver, res


def run_t5(cand, ch, rounds=100):
    def body(b, r, pipe):
        for _ in range(rounds):
            b.send("kmb active none")
            time.sleep(random.uniform(0.3, 1.0))
            b.send(f"kmb active {pipe}")
            b.send("kmb traffic single 1 0 0")
            time.sleep(0.4)
    bridge, receiver, _ = linked(cand, ch, body)
    for recs in (bridge, receiver):
        metrics.check_run(recs)
    fit = clockfit.fit(clockfit.pairs(bridge, receiver))
    d = metrics.switch_delays(bridge, receiver, fit)
    lat = metrics.latency(bridge, receiver, fit)
    return bridge, receiver, {"p50": clockfit.percentile(d, 50),
                              "p95": clockfit.percentile(d, 95),
                              "max": max(d) if d else float("nan"), "rounds": len(d),
                              "dups": lat.dups}


def run_t6(cand, ch):
    def body(b, r, p):
        traffic(b, "single 1000 20 200", 230)
        traffic(b, "stream 1000 10", 40)
    bridge, receiver, _ = linked(cand, ch, body, phantom=1)
    return bridge, receiver, latency_result(bridge, receiver)


def run_t7(cand, ch, rounds=20, load=False):
    """Receiver reboots `rounds` times; discovery from its Boot to its link.
    Under load (B only): the bridge streams to a phantom with no retries."""
    settings = ["kmb set retries_report 0", "kmb set retries_ping 0"] if load else []
    b, r = Recorder(BRIDGE), Recorder(RECEIVER)
    receiver = []
    try:
        pipe = start_link(b, r, cand, settings=[f"kmb set ch {ch}", *settings])
        if load:
            b.send("kmb phantom 1")
            time.sleep(0.2)
            b.send(f"kmb active {_phantom_pipe(pipe)}")
            b.send("kmb traffic stream 1000 3600")
        for _ in range(rounds):
            r.send("kmb reboot")
            try:
                receiver += r.stop()
            except (_port.PortClosed, _port.PortStalled):
                receiver += [x for _, x in r.records]
            time.sleep(2.5)  # the watchdog takes up to 2 s
            r = Recorder(RECEIVER, opener=lambda n: _port.reopen(n, timeout=10))
            est = lambda x: isinstance(x, Link) and x.event == LINK_ESTABLISHED
            r.wait_for(est, 5.0)
        r.send("kmb role none")
        b.send("kmb traffic stop")
    except BaseException:
        for x in (b, r):
            try:
                x.stop()
            except Exception:
                pass
        raise
    bridge, last = finish(b, r)
    receiver = unwrap_all(receiver + [x for x in last])
    metrics.check_run(receiver, allow_reboot=True)
    d = metrics.discovery_times(receiver)
    return bridge, receiver, {"p50": clockfit.percentile(d, 50),
                              "p95": clockfit.percentile(d, 95),
                              "max": max(d) if d else float("nan"), "rounds": len(d),
                              "load": load}


def _phantom_pipe(real_pipe):
    """`kmb phantom 1` takes the highest free pipe."""
    return 7 if real_pipe != 7 else 6


def run_t7b(cand, ch):
    def body(b, r, pipe):
        since = b.mark()
        t0 = time.monotonic()
        r.send("kmb role none")
        lost = lambda x: isinstance(x, Link) and x.event == LINK_LOST
        b.wait_for(lost, 5.0, since)
        for ht, x in b.records[since:]:
            if lost(x):
                return {"ms": (ht - t0) * 1000}
        return {"ms": float("nan")}
    bridge, receiver, extra = linked(cand, ch, body)
    return bridge, receiver, extra


def scan_busy(dwell_ms=200):
    """Combined busy permille per channel, both dongles scanning at once."""
    recs = [Recorder(BRIDGE), Recorder(RECEIVER)]
    marks = reset(*recs, settle=1.0)
    for x in recs:
        x.send(f"kmb scan {dwell_ms}")
    for x, m in zip(recs, marks):
        wait_text(x, "ok scan", m, 81 * dwell_ms / 1000 + 15)
    rows = [{s.channel: s for _, s in x.records[m:] if isinstance(s, Scan)}
            for x, m in zip(recs, marks)]
    for x in recs:
        x.stop()
    return {ch: rows[0][ch].busy_permille + rows[1][ch].busy_permille for ch in range(81)}


def run_t8(cand, ch=None, n=20_000):
    """Scan, then T4 cut to n reports on the 3 quietest spaced channels."""
    from .report import pick_channels
    busy = scan_busy()
    chans = pick_channels(busy, k=3, spacing=5)
    per = {}
    for c in chans:
        _, _, res = run_t4(cand, c, n=n)
        per[str(c)] = {"busy_permille": busy[c], "loss_per_10k": res["loss_per_10k"],
                       "p99": res["p99"], "dups": res["dups"]}
        print(f"  T8 ch {c}: busy {busy[c]}‰, loss/10k {res['loss_per_10k']:.3g}, "
              f"p99 {res['p99']:.0f} µs")
    return [], [], {"channels": per, "busy": busy}


TESTS = {"T1": run_t1, "T2": run_t2, "T3": run_t3, "T4": run_t4, "T5": run_t5,
         "T6": run_t6, "T7": run_t7, "T7b": run_t7b, "T8": run_t8}


def run(test, cand, ch, **kw):
    bridge, receiver, result = TESTS[test](cand, ch, **kw)
    d, result = save(test, cand, ch, bridge, receiver, result)
    print(f"{test} {cand} ch {ch}: " + ", ".join(
        f"{k} {v:.4g}" if isinstance(v, float) else f"{k} {v}" for k, v in result.items()
        if k not in ("test", "cand", "ch")))
    print(f"  saved {d}")
    return result


def latest_results(since=None):
    """The newest result.json per (test, candidate)."""
    out = {}
    for f in sorted(RESULTS.glob("*/result.json")):
        if since and f.parent.name < since:
            continue
        r = json.loads(f.read_text())
        out.setdefault(r["cand"], {})[r["test"]] = r
    return out
