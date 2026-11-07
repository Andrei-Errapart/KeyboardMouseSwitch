import pytest

from kmbench import records as r

# Frame sizes, type byte included, worked out by hand from the record
# table in the plan. The firmware must produce exactly these.
SIZES = {0x01: 39, 0x03: 47, 0x10: 10, 0x11: 23, 0x12: 17, 0x13: 13,
         0x14: 7, 0x15: 11, 0x16: 14}


@pytest.mark.parametrize("rtype,size", sorted(SIZES.items()))
def test_record_sizes_match_table(rtype, size):
    r.parse(bytes([rtype]) + bytes(size - 1))
    with pytest.raises(ValueError):
        r.parse(bytes([rtype]) + bytes(size - 2))
    with pytest.raises(ValueError):
        r.parse(bytes([rtype]) + bytes(size))


def test_parse_txdone():
    frame = bytes.fromhex("11 04030201 07000000 01 03 02 01 19 10"
                          " 0D0C0B0A 44332211")
    assert r.parse(frame) == r.TxDone(
        t=0x01020304, id=7, pipe=1, kind=3, attempts=2, ok=1, len=25,
        ack_len=16, t_tx_addr=0x0A0B0C0D, t_rx_addr=0x11223344)


def test_parse_rxpkt_signed_rssi():
    frame = bytes.fromhex("12 10000000 05000000 02 03 19 C4 20000000")
    assert r.parse(frame) == r.RxPkt(t=16, id=5, pipe=2, kind=3, len=25,
                                     rssi=-60, t_rx_addr=32)


def test_parse_boot_strings():
    frame = (bytes.fromhex("01 0A000000 EFBEADDE 01 42") + b"E1380B5925A4"
             + b"g1234567-dirty\0\0")
    assert r.parse(frame) == r.Boot(t=10, boot_id=0xDEADBEEF, role=1,
                                    cand="B", serial="E1380B5925A4",
                                    fw="g1234567-dirty")


def test_parse_text():
    assert r.parse(b"\x02ok role") == r.Text(text="ok role")


def test_parse_merged():
    frame = bytes.fromhex("16 10000000 05000000 04000000 01")
    assert r.parse(frame) == r.Merged(t=16, id=5, into=4, pipe=1)


def test_parse_rejects_short_frame():
    with pytest.raises(ValueError):
        r.parse(bytes.fromhex("11 04030201 07000000 01 03 02 01 19 10"
                              " 0D0C0B0A 443322"))


def test_parse_rejects_unknown_type():
    with pytest.raises(ValueError):
        r.parse(b"\x7f\x00\x00")


def test_unwrap_across_wrap():
    u = r.Unwrapper()
    assert [u.unwrap(0xFFFFFF00), u.unwrap(0x100)] == [4294967040, 4294967552]


def test_unwrap_resets_on_boot():
    u = r.Unwrapper()
    u.unwrap(0xFFFFFF00)
    u.unwrap(0x100)
    u.reset()
    assert u.unwrap(0x200) == 0x200


def test_unwrap_stamp_before_wrap():
    u = r.Unwrapper()
    u.unwrap(0xFFFFFF00)
    u.unwrap(0x100)
    assert u.unwrap(0xFFFFFFF0) == 4294967280


def test_unwrap_all_handles_stamps_and_boots():
    recs = [
        r.TxDone(t=0xFFFFFF00, id=1, pipe=1, kind=3, attempts=1, ok=1,
                 len=25, ack_len=16, t_tx_addr=0xFFFFFE00,
                 t_rx_addr=0xFFFFFE80),
        r.TxDone(t=0x100, id=2, pipe=1, kind=3, attempts=1, ok=1, len=25,
                 ack_len=16, t_tx_addr=0xFFFFFFF0, t_rx_addr=0x10),
        r.Boot(t=5, boot_id=1, role=2, cand="B", serial="X", fw="f"),
        r.Queued(t=7, id=1, pipe=1),
    ]
    out = r.unwrap_all(recs)
    assert (out[1].t, out[1].t_tx_addr, out[1].t_rx_addr) == (
        0x100000100, 0xFFFFFFF0, 0x100000010)
    assert out[3].t == 7
