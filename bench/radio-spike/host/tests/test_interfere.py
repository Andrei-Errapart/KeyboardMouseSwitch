from kmbench import interfere


def test_jam_report_parses_firmware_text():
    # Exactly what jam.c prints when the interferer stops.
    assert interfere.jam_report("ok jam off: ch 45, 1234 packets, 19.8 % on air") == (
        45, 1234, 19.8)


def test_jam_report_ignores_other_text():
    assert interfere.jam_report("ok jam off: not running") is None
    assert interfere.jam_report("ok jam ch 45 20 %") is None


import pytest

from kmbench import metrics
from kmbench.records import Stats


def _stats(t, dropped=0):
    return Stats(t=t, addr_tx=0, addr_rx=0, rec_dropped=dropped, stamp_lost=0, mic_fail=0,
                 replay_reject=0, merged=0, queue_dropped=0, mode_switches=0,
                 mode_switch_us_total=0, mode_switch_us_max=0)


@pytest.mark.parametrize("bad", ["bridge", "receiver"])
def test_discovery_result_checks_bridge_and_active_receiver(bad):
    # The third-dongle discovery run also reports the active receiver's
    # latency and loss: records dropped on CDC there must void the run, as
    # in every other run, not show up as radio loss.
    ok, dropped = [_stats(1), _stats(2)], [_stats(1), _stats(2, dropped=5)]
    records = {"bridge": ok, "receiver": ok, bad: dropped}
    with pytest.raises(metrics.RunInvalid):
        interfere.discovery_result(records["bridge"], records["receiver"], ok)


def test_discovery_resets_the_third_dongle_before_linking(monkeypatch):
    # A third dongle left a receiver by an aborted run (`kmb reboot` keeps
    # the role) links first and takes the pipe meant for the real receiver.
    import time
    events = []

    class Fake:
        def __init__(self, name, opener=None):
            self.name = name

        def send(self, line):
            events.append((self.name, line))

        def mark(self):
            return 0

        def stop(self):
            return []

    def start_link(b, r, cand, **kw):
        events.append(("start_link",))
        raise RuntimeError("stop here")

    monkeypatch.setattr(interfere.runs, "Recorder", Fake)
    monkeypatch.setattr(interfere.runs, "start_link", start_link)
    monkeypatch.setattr(time, "sleep", lambda s: None)
    with pytest.raises(RuntimeError):
        interfere.discovery("A", 45, rounds=1)
    before = events[:events.index(("start_link",))]
    assert (interfere.THIRD, "kmb role none") in before
