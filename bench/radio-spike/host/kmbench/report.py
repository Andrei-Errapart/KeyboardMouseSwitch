"""Pass/fail against the spec's bars, the A/B decision, and a markdown report.

`results` for one candidate maps a test id ("T1", ..., "T7b") to its metrics;
for the decision and the report, a dict maps "A"/"B" to such results.
"""
import math
from dataclasses import dataclass

LATENCY_BAR_US = 1000.0     # "about 1 ms or less over the air"
LOSS_BAR_PER_10K = 1.0      # T4: at most 1 report in 10,000
DISCOVERY_BAR_MS = 1000.0   # "known to the hub within about a second"
LIVENESS_BAR_MS = 1200.0    # link loss after 1 s, plus margin

TITLES = {
    "T1": "single reports", "T2": "load", "T3": "idle airtime", "T4": "reliability",
    "T5": "switch delay", "T6": "absent receiver", "T7": "discovery", "T7b": "liveness",
    "T8": "channels",
}
COLUMNS = ["p50", "p95", "p99", "max", "delivered", "lost", "dups", "loss_per_10k",
           "merged", "queue_dropped", "busy_fraction", "transaction_us",
           "busy_fraction_seven", "ms", "retries"]


@dataclass(frozen=True)
class Verdict:
    name: str
    value: float | None
    limit: float
    ok: bool


# (test, metric, name, limit): passes when value <= limit.
BARS = [
    ("T1", "p99", "T1 latency p99 µs", LATENCY_BAR_US),
    ("T2", "p99", "T2 latency p99 µs", LATENCY_BAR_US),
    ("T4", "p99", "T4 latency p99 µs", LATENCY_BAR_US),
    ("T6", "p99", "T6 latency p99 µs", LATENCY_BAR_US),
    ("T4", "loss_per_10k", "T4 loss per 10,000", LOSS_BAR_PER_10K),
    ("T7", "p95", "T7 discovery p95 ms", DISCOVERY_BAR_MS),
    ("T7b", "ms", "T7b liveness ms", LIVENESS_BAR_MS),
] + [(t, "dups", f"{t} dups", 0) for t in ("T1", "T2", "T4", "T5", "T6")]


def judge(results: dict) -> list:
    out = []
    for test, metric, name, limit in BARS:
        value = results.get(test, {}).get(metric)
        ok = value is not None and not (isinstance(value, float) and math.isnan(value)) \
            and value <= limit
        out.append(Verdict(name, value, limit, ok))
    return out


def decide(by_cand: dict) -> str | None:
    """B if B passes; otherwise A if A passes; otherwise neither (spec)."""
    for cand in ("B", "A"):
        if cand in by_cand and all(v.ok for v in judge(by_cand[cand])):
            return cand
    return None


def _fmt(v):
    if v is None:
        return "—"
    if isinstance(v, float):
        return "nan" if math.isnan(v) else f"{v:.4g}"
    return str(v)


def markdown(by_cand: dict) -> str:
    cands = sorted(by_cand, key=lambda c: "BA".index(c) if c in "BA" else 9)
    lines = ["# Radio bench results", ""]
    tests = sorted({t for r in by_cand.values() for t in r},
                   key=lambda t: list(TITLES).index(t) if t in TITLES else 99)
    for test in tests:
        cols = [c for c in COLUMNS if any(c in by_cand[k].get(test, {}) for k in cands)]
        lines += [f"### {test} {TITLES.get(test, '')}".rstrip(), "",
                  "| Candidate | " + " | ".join(cols) + " |",
                  "|---|" + "---|" * len(cols)]
        for c in cands:
            m = by_cand[c].get(test)
            if m is not None:
                lines.append(f"| {c} | " + " | ".join(_fmt(m.get(col)) for col in cols) + " |")
        lines.append("")
    lines += ["### Bars", "", "| Bar | Limit | " + " | ".join(cands) + " |",
              "|---|---|" + "---|" * len(cands)]
    verdicts = {c: judge(by_cand[c]) for c in cands}
    for i, (_, _, name, limit) in enumerate(BARS):
        cells = [f"{_fmt(verdicts[c][i].value)} {'ok' if verdicts[c][i].ok else 'FAIL'}"
                 for c in cands]
        lines.append(f"| {name} | {_fmt(limit)} | " + " | ".join(cells) + " |")
    choice = decide(by_cand)
    lines += ["", f"Decision: {choice}" if choice else
              "Decision: neither candidate passes every bar", ""]
    return "\n".join(lines)


def pick_channels(busy: dict, k: int = 3, spacing: int = 5) -> list:
    """The k quietest channels (lowest combined busy permille), each at
    least `spacing` channels from the others; quietest first."""
    chosen = []
    for ch in sorted(busy, key=lambda c: (busy[c], c)):
        if all(abs(ch - c) >= spacing for c in chosen):
            chosen.append(ch)
            if len(chosen) == k:
                break
    return chosen
