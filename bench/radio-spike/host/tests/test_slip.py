from kmbench import slip


def test_slip_roundtrip():
    a = bytes([0x01, 0xC0, 0x02, 0xDB, 0x03])
    b = bytes([0xDB, 0xDC, 0xC0, 0xDD])
    stream = slip.encode(a) + slip.encode(b)
    # Every split point, so an escape byte also lands at a chunk boundary.
    for cut in range(len(stream) + 1):
        d = slip.Decoder()
        assert d.feed(stream[:cut]) + d.feed(stream[cut:]) == [a, b]


def test_slip_encoding_escapes():
    assert slip.encode(bytes([0xC0, 0xDB, 0x41])) == bytes(
        [0xDB, 0xDC, 0xDB, 0xDD, 0x41, 0xC0])


def test_slip_ignores_empty_frames():
    assert slip.Decoder().feed(bytes([0xC0, 0xC0, 0x41, 0xC0])) == [b"A"]
