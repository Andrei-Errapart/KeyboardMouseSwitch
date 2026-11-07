"""Experiments with a third dongle, beyond the plan's T1-T8 (owner, 2026-11-07).

The third dongle ("macmini2", beside the Mac mini's receiver) either joins
as a second receiver while the bridge streams to the first, or interferes
with `kmb jam`: 25-byte dummy packets on a chosen channel, a chosen share
of the time, at a chosen power.

    python3 -m kmbench.interfere discovery A|B [--ch 45]
    python3 -m kmbench.interfere jam A|B --pct 20 --dbm 0 [--ch 45] [--jam-ch 45]
"""
import argparse
import pickle
import re
import time

from . import clockfit, metrics, port as _port, runs
from .records import KIND_POLL, KIND_REPORT, Link, LINK_ESTABLISHED, Text, TxDone, unwrap_all

THIRD = "macmini2"
_JAM_OFF = re.compile(r"ok jam off: ch (\d+), (\d+) packets, ([\d.]+) % on air")


def jam_report(text: str):
    """(channel, packets, % on air) from the interferer's closing text."""
    m = _JAM_OFF.fullmatch(text)
    return (int(m[1]), int(m[2]), float(m[3])) if m else None


def first_try(bridge, receiver, cand) -> float:
    """Share of data packets that got through on the first attempt: the
    bridge's reports for B, the receiver's polls for A."""
    sender, kind = (bridge, KIND_REPORT) if cand == "B" else (receiver, KIND_POLL)
    tx = [x for x in sender if isinstance(x, TxDone) and x.kind == kind]
    return sum(1 for x in tx if x.ok and x.attempts == 1) / len(tx) if tx else float("nan")


def jam_point(cand, ch, jam_ch, pct, dbm, n=300):
    """Link on `ch`, start the interferer, send `n` single reports 10-30 ms
    apart, stop the interferer. Linking happens before the interference."""
    j = runs.Recorder(THIRD)
    mark = runs.reset(j, settle=0.5)[0]
    j.send(f"kmb set power {dbm}")

    def body(b, r, p):
        j.send(f"kmb jam {jam_ch} {pct}")
        time.sleep(0.3)
        runs.traffic(b, f"single {n} 10 30", n * 0.03 + 30)
        j.send("kmb jam off")

    try:
        bridge, receiver, _ = runs.linked(cand, ch, body)
        off = j.wait_for(lambda x: isinstance(x, Text) and jam_report(x.text), 3.0, mark)
    finally:
        j.send("kmb set defaults")
        j.stop()
    res = runs.latency_result(bridge, receiver)
    res.update(jam_ch=jam_ch, jam_percent=pct, jam_dbm=dbm,
               jam_on_air=jam_report(off.text)[2] if off else float("nan"),
               first_try=first_try(bridge, receiver, cand))
    return bridge, receiver, res


def discovery(cand, ch, rounds=20):
    """The bridge streams 1000 reports/s to the active receiver while the
    third dongle reboots `rounds` times; ms from its Boot to its link."""
    b, r, x = (runs.Recorder(n) for n in (runs.BRIDGE, runs.RECEIVER, THIRD))
    third = []
    try:
        pipe = runs.start_link(b, r, cand, settings=[f"kmb set ch {ch}"])
        since = x.mark()
        x.send("kmb role none")
        x.send("kmb set defaults")
        x.send(f"kmb set ch {ch}")
        b.send(f"kmb active {pipe}")
        b.send("kmb traffic stream 1000 3600")
        x.send(f"kmb role receiver {cand}")
        est = lambda rec: isinstance(rec, Link) and rec.event == LINK_ESTABLISHED
        x.wait_for(est, 5.0, since)
        for _ in range(rounds):
            x.send("kmb reboot")
            try:
                third += x.stop()
            except (_port.PortClosed, _port.PortStalled):
                third += [rec for _, rec in x.records]
            time.sleep(2.5)  # the watchdog takes up to 2 s
            x = runs.Recorder(THIRD, opener=lambda n: _port.reopen(n, timeout=10))
            x.wait_for(est, 5.0)
        b.send("kmb traffic stop")
        x.send("kmb role none")
        x.send("kmb stats")  # every run's records end with a STATS
        time.sleep(1.5)
        third += x.stop()
    except BaseException:
        for y in (b, r, x):
            try:
                y.stop()
            except Exception:
                pass
        raise
    bridge, receiver = runs.finish(b, r)
    return bridge, receiver, unwrap_all(third)


def discovery_result(bridge, receiver, third) -> dict:
    """The third dongle's discovery times, and how the active receiver
    fared meanwhile. All three record streams must pass check_run."""
    for recs in (bridge, receiver):
        metrics.check_run(recs)
    metrics.check_run(third, allow_reboot=True)
    d = metrics.discovery_times(third)
    lat = metrics.latency(bridge, receiver, clockfit.fit(clockfit.pairs(bridge, receiver)))
    return {"p50": clockfit.percentile(d, 50), "p95": clockfit.percentile(d, 95),
            "max": max(d) if d else float("nan"), "rounds": len(d),
            "active_p99": lat.p99, "active_delivered": lat.delivered,
            "active_lost": lat.lost,
            "active_lost_after_ack": len(metrics.lost_after_ack(bridge, receiver)),
            "queue_dropped": runs.counter_rise(bridge, "queue_dropped")}


def _fmt(res):
    return ", ".join(f"{k} {v:.4g}" if isinstance(v, float) else f"{k} {v}"
                     for k, v in res.items())


def main(argv=None):
    ap = argparse.ArgumentParser(prog="kmbench.interfere")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("discovery")
    d.add_argument("cand", choices=["A", "B"])
    d.add_argument("--ch", type=int, default=45)
    j = sub.add_parser("jam")
    j.add_argument("cand", choices=["A", "B"])
    j.add_argument("--ch", type=int, default=45)
    j.add_argument("--jam-ch", type=int)
    j.add_argument("--pct", type=int, required=True)
    j.add_argument("--dbm", type=int, default=0)
    j.add_argument("--n", type=int, default=300)
    args = ap.parse_args(argv)
    third = None
    if args.cmd == "discovery":
        bridge, receiver, third = discovery(args.cand, args.ch)
        res = discovery_result(bridge, receiver, third)
        name = "T7third"
    else:
        jam_ch = args.ch if args.jam_ch is None else args.jam_ch
        bridge, receiver, res = jam_point(args.cand, args.ch, jam_ch, args.pct, args.dbm, args.n)
        name = f"J{jam_ch}-{args.pct}pct-{args.dbm}dBm"
    out, _ = runs.save(name, args.cand, args.ch, bridge, receiver, res)
    if third is not None:
        with open(out / "third.pkl", "wb") as f:
            pickle.dump(third, f)
    print(f"{name} {args.cand} ch {args.ch}: {_fmt(res)}")
    print(f"  saved {out}")


if __name__ == "__main__":
    main()
