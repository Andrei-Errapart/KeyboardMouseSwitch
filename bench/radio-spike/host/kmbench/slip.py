"""SLIP framing (RFC 1055) for the dongles' record stream."""
END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD


def encode(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b == END:
            out += bytes([ESC, ESC_END])
        elif b == ESC:
            out += bytes([ESC, ESC_ESC])
        else:
            out.append(b)
    out.append(END)
    return bytes(out)


def _unescape(raw: bytes) -> bytes:
    out, i = bytearray(), 0
    while i < len(raw):
        b = raw[i]
        if b == ESC:
            if i + 1 < len(raw):
                nxt = raw[i + 1]
                out.append({ESC_END: END, ESC_ESC: ESC}.get(nxt, nxt))
            i += 2
        else:
            out.append(b)
            i += 1
    return bytes(out)


class Decoder:
    """Splits a byte stream into frames; keeps partial frames across feeds."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> list:
        frames = []
        for b in data:
            if b == END:
                if self._buf:
                    frames.append(_unescape(self._buf))
                    self._buf = bytearray()
            else:
                self._buf.append(b)
        return frames
