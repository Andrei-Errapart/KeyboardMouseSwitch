import random

import pytest

from kmbench import clockfit
from kmbench.records import RxPkt, TxDone

REPORT, POLL = 3, 6


def _pairs(n, offset, drift_ppm, noise, seed=1):
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        tb = 1_000_000 + i * 120_000          # every 120 ms over 60 s
        tr = tb * (1 + drift_ppm / 1e6) + offset + rnd.uniform(-noise, noise)
        out.append((tb, round(tr)))
    return out


def test_fit_recovers_offset_and_drift():
    f = clockfit.fit(_pairs(500, 123456, 25, 3))
    assert abs(f.offset_us - 123456) < 2
    assert abs(f.drift_ppm - 25) < 1
    assert f.n == 500


def test_fit_to_bridge_inverts_the_model():
    f = clockfit.Fit(offset_us=1000.0, drift_ppm=100.0, resid_p99_us=1.0, n=50)
    # t_receiver = 2_000_000 * 1.0001 + 1000 = 2_001_200
    assert f.to_bridge(2_001_200) == pytest.approx(2_000_000, abs=1e-6)


def test_fit_refuses_few_pairs():
    with pytest.raises(clockfit.ClockFitError):
        clockfit.fit(_pairs(19, 0, 0, 1))


def test_fit_refuses_noise():
    with pytest.raises(clockfit.ClockFitError):
        clockfit.fit(_pairs(500, 0, 0, 50))


def _txdone(id, attempts, t_tx, kind=REPORT, ok=1):
    return TxDone(t=t_tx + 300, id=id, pipe=1, kind=kind, attempts=attempts,
                  ok=ok, len=25, ack_len=16, t_tx_addr=t_tx,
                  t_rx_addr=t_tx + 250)


def _rxpkt(id, t_rx, kind=REPORT):
    return RxPkt(t=t_rx + 120, id=id, pipe=1, kind=kind, len=25, rssi=-50,
                 t_rx_addr=t_rx)


def test_pairs_first_attempt_only():
    bridge = [_txdone(1, 1, 1000), _txdone(2, 2, 5000)]
    receiver = [_rxpkt(1, 9000), _rxpkt(2, 13435)]
    assert clockfit.pairs(bridge, receiver) == [(pytest.approx(999.4), pytest.approx(8995.0))]


def test_pairs_skip_failed_transmissions():
    bridge = [_txdone(1, 1, 1000, ok=0)]
    receiver = [_rxpkt(1, 9000)]
    assert clockfit.pairs(bridge, receiver) == []


def test_pairs_from_receiver_polls():
    # Candidate A: the receiver transmits polls; the bridge receives them.
    # Pairs are still (bridge time, receiver time).
    receiver = [_txdone(7, 1, 20000, kind=POLL)]
    bridge = [_rxpkt(7, 12000, kind=POLL)]
    assert clockfit.pairs(bridge, receiver) == [(pytest.approx(11995.0), pytest.approx(19999.4))]


def test_pairs_correct_for_radio_chain_delays():
    # The nRF52840 raises ADDRESS 0.6 µs after the address leaves the
    # antenna when sending and 5.0 µs after it arrives when receiving, at
    # 2 Mbit/s (Zephyr's radio_nrf52840.h). Uncorrected, pairs that all run
    # one way bias the clock offset by 4.4 µs. One on-air instant at which
    # both clocks read 1000.0 must give the pair (1000.0, 1000.0).
    bridge = [_txdone(1, 1, 1000.6)]
    receiver = [_rxpkt(1, 1005.0)]
    assert clockfit.pairs(bridge, receiver) == [(pytest.approx(1000.0), pytest.approx(1000.0))]


def test_pairs_need_same_kind():
    bridge = [_txdone(1, 1, 1000, kind=REPORT)]
    receiver = [_rxpkt(1, 9000, kind=POLL)]
    assert clockfit.pairs(bridge, receiver) == []


def test_pairs_skip_unknown_stamps():
    # The firmware zeroes both stamps when it couldn't tell an ADDRESS
    # event's direction, so a stale stamp never makes a false pair.
    bridge = [_txdone(1, 1, 0), _txdone(2, 1, 5000)]
    receiver = [_rxpkt(1, 9000), _rxpkt(2, 0)]
    assert clockfit.pairs(bridge, receiver) == []
