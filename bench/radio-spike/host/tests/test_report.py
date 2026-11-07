import pytest

from kmbench import report

# A run that sits exactly on every bar (the bars are inclusive).
GOOD = {
    "T1": {"p50": 400.0, "p99": 1300.0, "max": 1500.0, "delivered": 1000, "lost": 0, "dups": 0},
    "T2": {"p50": 500.0, "p99": 1300.0, "max": 1800.0, "delivered": 1500, "lost": 0, "dups": 0},
    "T4": {"p50": 450.0, "p99": 1300.0, "max": 3000.0, "delivered": 99990, "lost": 10,
           "dups": 0, "loss_per_10k": 1.0},
    "T5": {"p50": 3.0, "p95": 90.0, "max": 120.0, "dups": 0},
    "T6": {"p50": 420.0, "p99": 1300.0, "max": 1600.0, "delivered": 1000, "lost": 0, "dups": 0},
    "T7": {"p50": 120.0, "p95": 1000.0, "max": 1400.0},
    "T7b": {"ms": 1200.0},
}


def run(**changes):
    out = {t: dict(m) for t, m in GOOD.items()}
    for key, value in changes.items():
        test, metric = key.split("__")
        out[test][metric] = value
    return out


def test_judge_passes_good_run():
    verdicts = report.judge(GOOD)
    assert verdicts and all(v.ok for v in verdicts)


@pytest.mark.parametrize("key,value", [
    ("T1__p99", 1300.1), ("T2__p99", 1300.1), ("T4__p99", 1300.1),
    ("T1__dups", 1), ("T2__dups", 1), ("T4__dups", 1), ("T5__dups", 1), ("T6__dups", 1),
    ("T4__loss_per_10k", 1.01), ("T7__p95", 1000.1), ("T6__p99", 1300.1),
    ("T7b__ms", 1200.1),
])
def test_judge_fails_each_bar(key, value):
    failed = [v for v in report.judge(run(**{key: value})) if not v.ok]
    assert len(failed) == 1
    assert failed[0].name.startswith(key.split("__")[0])


def test_t5_has_no_bar():
    assert all(v.ok for v in report.judge(run(T5__p95=99999.0)))


def test_missing_test_fails_its_bars():
    results = run()
    del results["T7b"]
    failed = [v for v in report.judge(results) if not v.ok]
    assert [v.name for v in failed] == ["T7b liveness ms"]


def test_decision_prefers_B_when_both_pass():
    assert report.decide({"A": GOOD, "B": GOOD}) == "B"


def test_decision_A_when_B_fails():
    assert report.decide({"A": GOOD, "B": run(T1__p99=2000.0)}) == "A"


def test_decision_none_when_both_fail():
    assert report.decide({"A": run(T4__dups=2), "B": run(T1__p99=2000.0)}) is None


def test_markdown_has_table_per_test():
    md = report.markdown({"A": GOOD, "B": run(T1__p99=2000.0)})
    for test in ("T1", "T2", "T4", "T5", "T6", "T7", "T7b"):
        assert f"### {test}" in md
    assert "| B |" in md and "| A |" in md
    assert "Decision: A" in md


def test_pick_channels_quietest_spaced():
    # combined busy permille per channel, both dongles
    busy = {ch: 500 for ch in range(81)}
    busy.update({2: 10, 3: 5, 4: 8, 40: 30, 41: 20, 62: 15, 80: 12})
    # 3 is quietest; 2 and 4 are too close to it; then 80, 62.
    assert report.pick_channels(busy, k=3, spacing=5) == [3, 80, 62]


def test_pick_channels_respects_spacing_at_the_band_edge():
    busy = {ch: 100 - ch for ch in range(81)}   # quieter towards 80
    assert report.pick_channels(busy, k=3, spacing=5) == [80, 75, 70]
