#!/usr/bin/env python3
"""Report an nRF52840 Dongle's chip revision and bootloader, read-only.

The dongle must be in its Nordic Open DFU bootloader (USB 1915:521f): a
dongle with no application, or one put there by the reset button or our
firmware's reboot command. The script talks the bootloader's serial DFU
protocol and sends only query opcodes: PING, PROTOCOL_VERSION, MTU_GET,
HARDWARE_VERSION and FIRMWARE_VERSION. It never sends CREATE, WRITE,
EXECUTE, SELECT or ABORT, so it can't change the dongle.

The chip revision decides how the debug port is locked (see "Key handling"
in docs/superpowers/specs/2026-11-04-keyboard-mouse-switch-design.md), so
every new dongle is checked with this before its first flash.

Usage:
    tools/dongle-info.py /dev/cu.usbmodem<serial>1     (macOS)
    tools/dongle-info.py /dev/ttyACM0                  (Linux)
"""
import argparse
import glob
import os
import select
import struct
import sys
import termios
import time
import tty

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD

RESULTS = {0x00: "INVALID", 0x01: "SUCCESS", 0x02: "OP_NOT_SUPPORTED",
           0x03: "INVALID_PARAMETER", 0x04: "INSUFFICIENT_RESOURCES",
           0x05: "INVALID_OBJECT", 0x07: "UNSUPPORTED_TYPE",
           0x08: "OPERATION_NOT_PERMITTED", 0x0A: "OPERATION_FAILED",
           0x0B: "EXT_ERROR"}

FW_TYPES = {0: "softdevice", 1: "application", 2: "bootloader",
            0xFF: "none"}

# Third character of the FICR variant (the build code letter) to nRF52840
# IC revision, from Nordic's nRF52840 compatibility matrix.
REVISIONS = {
    "C": ("1", "older: debug port open unless UICR.APPROTECT is written"),
    "D": ("2", "older: debug port open unless UICR.APPROTECT is written"),
    "F": ("3", "newer: debug port locked after every reset unless "
               "firmware opens it"),
}


def slip(data):
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


def unslip(buf):
    out, i = bytearray(), 0
    while i < len(buf):
        if buf[i] == ESC and i + 1 < len(buf):
            out.append(END if buf[i + 1] == ESC_END else ESC)
            i += 2
        else:
            out.append(buf[i])
            i += 1
    return bytes(out)


def request(fd, req, timeout=1.5):
    """Send one request; return its payload on success, else None."""
    os.write(fd, slip(req))
    buf, t0 = bytearray(), time.time()
    while time.time() - t0 < timeout:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            buf += os.read(fd, 512)
        except BlockingIOError:
            continue
        while END in buf:
            frame, buf = buf[:buf.index(END)], buf[buf.index(END) + 1:]
            if not frame:
                continue
            resp = unslip(frame)
            if len(resp) >= 3 and resp[0] == 0x60 and resp[1] == req[0]:
                if resp[2] == 0x01:
                    return resp[3:]
                result = RESULTS.get(resp[2], hex(resp[2]))
                print(f"  opcode {req[0]:#04x}: {result}", file=sys.stderr)
                return None
    print(f"  opcode {req[0]:#04x}: no response", file=sys.stderr)
    return None


def candidates():
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


def main():
    parser = argparse.ArgumentParser(
        description="Read-only query of an nRF52840 Dongle in its Open DFU "
                    "bootloader: chip revision and installed images.")
    parser.add_argument("port", nargs="?",
                        help="serial port of the dongle's bootloader")
    args = parser.parse_args()
    if not args.port:
        ports = candidates()
        print("Give the dongle's serial port. Candidates:", file=sys.stderr)
        for p in ports or ["(none found)"]:
            print(f"  {p}", file=sys.stderr)
        return 2

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        termios.tcflush(fd, termios.TCIOFLUSH)

        if request(fd, b"\x09\x5a") != b"\x5a":
            print(f"{args.port}: no DFU bootloader answering",
                  file=sys.stderr)
            return 1
        proto = request(fd, b"\x00")
        print(f"port:              {args.port}")
        if proto:
            print(f"DFU protocol:      {proto[0]}")

        hw = request(fd, b"\x0a")
        if hw and len(hw) >= 20:
            part, variant, rom, ram, page = struct.unpack("<5I", hw[:20])
            vstr = variant.to_bytes(4, "big").decode("ascii", "replace")
            rev, approtect = REVISIONS.get(
                vstr[2:3], ("unknown", "check Nordic's compatibility matrix"))
            print(f"chip:              nRF{part:x}, variant {vstr}")
            print(f"revision:          {rev}")
            print(f"debug port:        {approtect}")
            print(f"flash / RAM:       {rom // 1024} KB / {ram // 1024} KB, "
                  f"{page // 1024} KB pages")

        for image in range(4):
            fw = request(fd, bytes([0x0B, image]))
            if not fw or len(fw) < 13:
                continue
            ftype = fw[0]
            version, addr, length = struct.unpack("<3I", fw[1:13])
            if ftype == 0xFF:
                continue
            print(f"image {image}:           "
                  f"{FW_TYPES.get(ftype, hex(ftype))}, version {version}, "
                  f"at {addr:#07x}, {length // 1024} KB")
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
