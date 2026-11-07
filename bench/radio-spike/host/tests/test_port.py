import os
import sys
import time
import tty

import pytest

from kmbench import port, slip
from kmbench.records import Stats

PY = sys.executable

STATS_FRAME = slip.encode(bytes([0x03]) + bytes(46))


def fake(script):
    """A Transport running a small Python program as the 'dongle'."""
    return port.Transport.from_command([PY, "-c", script])


def test_port_closed_on_eof():
    p = port.Port(fake("pass"))
    start = time.monotonic()
    with pytest.raises(port.PortClosed):
        for _ in p.records(timeout=5):
            pass
    assert time.monotonic() - start < 2


def test_port_stalled_without_stats():
    p = port.Port(fake("import time; time.sleep(10)"))
    start = time.monotonic()
    with pytest.raises(port.PortStalled):
        for _ in p.records(timeout=10):
            pass
    assert time.monotonic() - start < 3
    p.close()


def test_port_yields_records_and_stops_at_timeout():
    script = ("import sys, time\n"
              f"frame = {STATS_FRAME!r}\n"
              "for _ in range(20):\n"
              "    sys.stdout.buffer.write(frame); sys.stdout.flush()\n"
              "    time.sleep(0.1)\n"
              "time.sleep(5)\n")
    p = port.Port(fake(script))
    got = list(p.records(timeout=1.0))
    p.close()
    assert 5 <= len(got) <= 12
    assert all(isinstance(r, Stats) for r in got)


def test_port_send_writes_a_kmb_line():
    script = ("import sys\n"
              "line = sys.stdin.buffer.readline()\n"
              "sys.stdout.buffer.write(bytes([0xC0, 0x02]) + line.strip() "
              "+ bytes([0xC0])); sys.stdout.flush()\n")
    p = port.Port(fake(script))
    p.send("kmb info")
    with pytest.raises(port.PortClosed):
        got = []
        for rec in p.records(timeout=3):
            got.append(rec)
    assert [r.text for r in got] == ["kmb info"]


def test_from_device_keeps_bytes_already_waiting():
    # Opening the dongle's port raises DTR, and the dongle answers at once
    # with the records it held back (the Boot record after a reboot). They
    # may arrive before from_device() returns; it must not flush them.
    master, slave = os.openpty()
    tty.setraw(slave)
    os.write(master, STATS_FRAME)
    p = port.Port(port.Transport.from_device(os.ttyname(slave)))
    try:
        assert isinstance(p.first(1), Stats)
    finally:
        p.close()
        os.close(slave)
        os.close(master)


def test_close_lets_the_command_finish(tmp_path):
    # Like ssh + socat: killing the local ssh leaves the remote socat
    # holding the serial port for a moment. close() must end the command by
    # closing its input and waiting, so the port is free when it returns.
    marker = tmp_path / "done"
    t = fake("import sys, time, pathlib\n"
             "sys.stdin.buffer.read()\n"
             "time.sleep(0.3)\n"
             f"pathlib.Path({str(marker)!r}).write_text('x')\n")
    t.close()
    assert marker.exists()


def test_reopen_after_reboot():
    attempts = []

    def opener(name):
        attempts.append(name)
        if len(attempts) < 3:
            return fake("import sys; sys.exit(1)")
        return fake("import sys, time\n"
                    f"sys.stdout.buffer.write({STATS_FRAME!r})\n"
                    "sys.stdout.flush(); time.sleep(5)\n")

    p = port.reopen("macmini", timeout=10, opener=opener)
    first = next(iter(p.records(timeout=2)))
    p.close()
    assert len(attempts) == 3
    assert isinstance(first, Stats)


def test_reopen_gives_up():
    with pytest.raises(port.PortClosed):
        port.reopen("macmini", timeout=1.5,
                    opener=lambda name: fake("import sys; sys.exit(1)"))


def test_wait_for_bootloader_detects_ping(monkeypatch):
    script = ("import sys\n"
              "req = b''\n"
              "while not req.endswith(bytes([0xC0])):\n"
              "    req += sys.stdin.buffer.read(1)\n"
              "assert req == bytes([0x09, 0x5A, 0xC0]), req\n"
              "sys.stdout.buffer.write(bytes([0xC0, 0x60, 0x09, 0x01, 0x5A, 0xC0]))\n"
              "sys.stdout.flush()\n"
              "import time; time.sleep(5)\n")
    monkeypatch.setattr(port, "port_path", lambda name: "/dev/fake")
    assert port.wait_for_bootloader(
        "macmini", timeout=5, opener=lambda name: fake(script)) == "/dev/fake"


def test_wait_for_bootloader_ignores_the_application():
    # The application never answers a DFU ping.
    with pytest.raises(TimeoutError):
        port.wait_for_bootloader(
            "macmini", timeout=2,
            opener=lambda name: fake("import time; time.sleep(5)"))


def test_dongle_table():
    assert port.DONGLES["macmini"].serial == "E1380B5925A4"
    assert port.DONGLES["macmini"].host is None
    assert port.DONGLES["macmini"].port_glob == "/dev/cu.usbmodemE1380B5925A4*"
    assert port.DONGLES["anduq7"].serial == "DA1EAB2D90C1"
    assert port.DONGLES["anduq7"].host == "anduq7"
    assert port.DONGLES["anduq7"].port_glob == (
        "/dev/serial/by-id/usb-*_DA1EAB2D90C1-if00")
    # The third dongle (for the MacBook Pro later) joins the bench on the
    # Mac mini as a second receiver or a controlled interferer.
    assert port.DONGLES["macmini2"].serial == "E2E8D954B732"
    assert port.DONGLES["macmini2"].host is None
    assert port.DONGLES["macmini2"].port_glob == "/dev/cu.usbmodemE2E8D954B732*"


def test_remote_command_uses_socat_with_keepalive():
    argv = port.remote_command(port.DONGLES["anduq7"])
    assert argv[:6] == ["ssh", "-T", "-o", "ServerAliveInterval=1", "-o",
                        "ServerAliveCountMax=2"]
    assert argv[-2] == "anduq7"
    assert "socat - \"$p\",raw,echo=0" in argv[-1]
    assert "/dev/serial/by-id/usb-*_DA1EAB2D90C1-if00" in argv[-1]
