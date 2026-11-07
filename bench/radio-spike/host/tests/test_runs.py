from kmbench import runs


class FakeRecorder:
    def __init__(self):
        self.sent = []

    def send(self, line):
        self.sent.append(line)

    def mark(self):
        return len(self.sent)


def test_reset_restores_default_settings():
    # `kmb set` values stay on the dongle across roles: a run that sets
    # retries_report 0 (T7 under load) would otherwise leave every later
    # run sending reports without retries.
    b, r = FakeRecorder(), FakeRecorder()
    runs.reset(b, r, settle=0)
    for x in (b, r):
        assert x.sent == ["kmb role none", "kmb set defaults"]


def test_latest_results_keeps_t7_under_load_apart(tmp_path, monkeypatch):
    # T7 under load saves as test "T7" with load true. As the newest T7 it
    # used to replace the plain T7, whose p95 the discovery bar judges.
    import json
    monkeypatch.setattr(runs, "RESULTS", tmp_path)
    for name, load, p95 in (("20261107T212448Z-T7-B", False, 2.5),
                            ("20261107T212905Z-T7-B", True, float("nan"))):
        (tmp_path / name).mkdir()
        (tmp_path / name / "result.json").write_text(
            json.dumps({"test": "T7", "cand": "B", "p95": p95, "load": load}))
    got = runs.latest_results()
    assert got["B"]["T7"]["p95"] == 2.5
    assert got["B"]["T7load"]["load"] is True
