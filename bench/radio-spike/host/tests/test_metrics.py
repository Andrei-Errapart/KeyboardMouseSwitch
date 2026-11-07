import math

import pytest

from kmbench import metrics
from kmbench.clockfit import Fit
from kmbench.records import (Boot, Link, Merged, Queued, RxPkt, Stats, Text,
                             TxDone)

REPORT, STATUS = 3, 5
FIT = Fit(offset_us=123456.0, drift_ppm=0.0, resid_p99_us=1.0, n=100)


def rx(id, t, kind=REPORT):
    return RxPkt(t=t, id=id, pipe=1, kind=kind, len=25, rssi=-50,
                 t_rx_addr=t - 100)


def tx(attempts=1, ok=1, length=25, ack_len=16, t_tx=0, t_rx=0, id=1):
    return TxDone(t=t_rx + 50, id=id, pipe=1, kind=REPORT, attempts=attempts,
                  ok=ok, len=length, ack_len=ack_len, t_tx_addr=t_tx,
                  t_rx_addr=t_rx)


def stats(t, rec_dropped=0, stamp_lost=0):
    return Stats(t=t, addr_tx=0, addr_rx=0, rec_dropped=rec_dropped,
                 stamp_lost=stamp_lost, mic_fail=0, replay_reject=0, merged=0,
                 queue_dropped=0, mode_switches=0, mode_switch_us_total=0,
                 mode_switch_us_max=0)


def boot(t):
    return Boot(t=t, boot_id=1, role=2, cand="B", serial="E1380B5925A4",
                fw="f")


def test_latency_basic():
    lat = metrics.latency([Queued(t=1000, id=1, pipe=1)],
                          [rx(1, 123456 + 1350)], FIT)
    assert lat.samples_us == [350.0]
    assert (lat.delivered, lat.lost, lat.dups, lat.merged) == (1, 0, 0, 0)


def test_latency_counts_lost():
    lat = metrics.latency([Queued(t=1000, id=1, pipe=1),
                           Queued(t=2000, id=2, pipe=1)],
                          [rx(1, 123456 + 1350)], FIT)
    assert lat.lost == 1
    assert lat.delivered == 1


def test_latency_dup_counts_once():
    lat = metrics.latency([Queued(t=1000, id=1, pipe=1)],
                          [rx(1, 123456 + 1350), rx(1, 123456 + 1900)], FIT)
    assert lat.dups == 1
    assert lat.samples_us == [350.0]


def test_retransmit_is_not_dup():
    bridge = [Queued(t=1000, id=1, pipe=1), tx(attempts=3, id=1)]
    lat = metrics.latency(bridge, [rx(1, 123456 + 1350)], FIT)
    assert lat.dups == 0


def test_latency_merged_is_not_lost():
    bridge = [Queued(t=1000, id=4, pipe=1), Queued(t=1200, id=5, pipe=1),
              Merged(t=1200, id=5, into=4, pipe=1)]
    lat = metrics.latency(bridge, [rx(4, 123456 + 1500)], FIT)
    assert (lat.lost, lat.merged) == (0, 1)
    assert sorted(lat.samples_us) == [300.0, 500.0]


def test_latency_follows_merge_chains():
    bridge = [Queued(t=1000, id=4, pipe=1), Queued(t=1100, id=5, pipe=1),
              Queued(t=1200, id=6, pipe=1),
              Merged(t=1100, id=5, into=4, pipe=1),
              Merged(t=1200, id=6, into=5, pipe=1)]
    lat = metrics.latency(bridge, [rx(4, 123456 + 1500)], FIT)
    assert (lat.lost, lat.merged, len(lat.samples_us)) == (0, 2, 3)


def test_latency_ignores_reports_not_queued():
    # Refreshes are REPORTs the traffic generator didn't queue.
    lat = metrics.latency([Queued(t=1000, id=1, pipe=1)],
                          [rx(1, 123456 + 1350), rx(9, 123456 + 5000)], FIT)
    assert (lat.delivered, lat.dups) == (1, 0)


def test_latency_percentiles():
    queued = [Queued(t=0, id=i, pipe=1) for i in range(1, 101)]
    got = [rx(i, 123456 + i * 10) for i in range(1, 101)]   # 10..1000 µs
    lat = metrics.latency(queued, got, FIT)
    assert (lat.p50, lat.p99, lat.max) == (500.0, 990.0, 1000.0)


def test_latency_without_samples_is_nan():
    lat = metrics.latency([Queued(t=0, id=1, pipe=1)], [], FIT)
    assert math.isnan(lat.p99)


def test_airtime_reports():
    recs = [tx(length=25, ack_len=16) for _ in range(1000)]
    assert metrics.airtime(recs, 1.0) == pytest.approx(
        1000 * (140.5 + 104.5) / 1e6)


def test_airtime_failed_transmissions_have_no_ack():
    recs = [tx(attempts=2, ok=0, length=25, ack_len=0)]
    assert metrics.airtime(recs, 1.0) == pytest.approx(2 * 140.5 / 1e6)


def test_transaction_time():
    assert metrics.transaction_us([tx(t_tx=1000, t_rx=1250, ack_len=16)]) == [
        394.5]


def test_transaction_time_first_try_only():
    assert metrics.transaction_us([tx(attempts=2, t_tx=1000, t_rx=1700),
                                   tx(ok=0, t_tx=1000, t_rx=900)]) == []


def test_switch_delay():
    bridge = [Link(t=10_000, event=5, pipe=1, peer=bytes(6)),
              Link(t=500_000, event=6, pipe=1, peer=bytes(6)),
              Link(t=900_000, event=5, pipe=1, peer=bytes(6))]
    receiver = [rx(1, 123456 + 5_000),               # before activation
                rx(2, 123456 + 60_000),              # 50 ms after the first
                rx(3, 123456 + 70_000),
                rx(4, 123456 + 905_000, kind=STATUS),
                rx(5, 123456 + 912_500)]             # 12.5 ms after the second
    assert metrics.switch_delays(bridge, receiver, FIT) == [50.0, 12.5]


def test_discovery_time():
    recs = [boot(100), Link(t=350_100, event=2, pipe=1, peer=bytes(6)),
            Link(t=400_100, event=3, pipe=1, peer=bytes(6)),
            boot(50), Link(t=250_050, event=3, pipe=1, peer=bytes(6)),
            boot(80)]                                  # never linked
    assert metrics.discovery_times(recs) == [400.0, 250.0]


def test_run_valid():
    metrics.check_run([stats(1), tx(), stats(2)])


def test_run_invalid_on_dropped_records():
    with pytest.raises(metrics.RunInvalid):
        metrics.check_run([stats(1, rec_dropped=0), tx(),
                           stats(2, rec_dropped=1)])


def test_run_valid_with_earlier_drops():
    metrics.check_run([stats(1, rec_dropped=5), tx(), stats(2, rec_dropped=5)])


def test_run_invalid_without_final_stats():
    with pytest.raises(metrics.RunInvalid):
        metrics.check_run([stats(1), tx(), stats(2), tx()])


def test_run_final_text_is_fine():
    metrics.check_run([stats(1), tx(), stats(2), Text(text="ok stats")])


def test_run_valid_despite_lost_stamps():
    # Lost stamps are zeroed in the firmware and skipped by the clock fit:
    # they cost pairs, not correctness.
    metrics.check_run([stats(1, stamp_lost=0), tx(), stats(2, stamp_lost=2)])


def test_run_invalid_on_reboot():
    with pytest.raises(metrics.RunInvalid):
        metrics.check_run([stats(1), boot(5), stats(2)])


def test_run_allowed_reboot_judges_each_boot():
    # Counters restart at boot; only a rise within one boot counts.
    metrics.check_run([stats(1, rec_dropped=7), boot(5),
                       stats(2, rec_dropped=0), tx(), stats(3, rec_dropped=0)],
                      allow_reboot=True)
    with pytest.raises(metrics.RunInvalid):
        metrics.check_run([stats(1), boot(5), stats(2, rec_dropped=0), tx(),
                           stats(3, rec_dropped=4)], allow_reboot=True)
