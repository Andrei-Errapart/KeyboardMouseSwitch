#!/usr/bin/env python3
"""Check that a dongle application image fits under the Open DFU bootloader.

The nRF52840 Dongle's factory bootloader starts applications at 0x1000 and
occupies flash from 0xE0000 up. An image built for the wrong layout would
overwrite the MBR or the bootloader, so every dongle image is checked before
its first DFU (spec risk 6). UICR data is rejected too: a DFU package can't
write UICR.

Usage:
    tools/check-dongle-image.py build/.../zephyr/zephyr.hex
"""
import sys

APP_START = 0x1000
BOOTLOADER_START = 0xE0000
UICR_START, UICR_END = 0x10001000, 0x10002000


def _segments(hex_path):
    """Contiguous (start, end) byte ranges of the data records."""
    segments, upper = [], 0
    with open(hex_path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError(f"line {lineno}: not an Intel HEX record")
            raw = bytes.fromhex(line[1:])
            count, rtype = raw[0], raw[3]
            data = raw[4:4 + count]
            if sum(raw) & 0xFF:
                raise ValueError(f"line {lineno}: bad checksum")
            if rtype == 0x00:
                start = (upper | int.from_bytes(raw[1:3], "big"))
                end = start + count
                if segments and segments[-1][1] == start:
                    segments[-1] = (segments[-1][0], end)
                else:
                    segments.append((start, end))
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                upper = int.from_bytes(data, "big") << 4
            elif rtype == 0x04:
                upper = int.from_bytes(data, "big") << 16
    return sorted(segments)


def _addr(a):
    return f"0x{a:08X}"


def check(hex_path):
    """Return the placement problems of an image; an empty list means fine."""
    segments = _segments(hex_path)
    if not segments:
        return ["the image contains no data"]
    problems = []
    for start, end in segments:
        if UICR_START <= start < UICR_END:
            problems.append(f"data at {_addr(start)} writes UICR, which a DFU "
                            "package can't carry")
        elif start < APP_START:
            problems.append(f"data at {_addr(start)} is below the application "
                            f"start {_addr(APP_START)} (MBR area)")
        elif end > BOOTLOADER_START:
            first = max(start, BOOTLOADER_START)
            problems.append(f"data at {_addr(first)} reaches the bootloader, "
                            f"which starts at {_addr(BOOTLOADER_START)}")
    lowest = segments[0][0]
    if not problems and lowest != APP_START:
        problems.append(f"the image starts at {_addr(lowest)}, not at "
                        f"{_addr(APP_START)}")
    return problems


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    problems = check(argv[1])
    if problems:
        for p in problems:
            print(p)
        return 1
    segments = _segments(argv[1])
    print(f"OK: {segments[0][0]:#x}-{max(e for _, e in segments):#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
