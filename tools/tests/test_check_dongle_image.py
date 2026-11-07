"""Tests for tools/check-dongle-image.py (spec risk 6: image placement)."""
import importlib.util
import pathlib
import subprocess
import sys

TOOL = pathlib.Path(__file__).resolve().parents[1] / "check-dongle-image.py"
_spec = importlib.util.spec_from_file_location("check_dongle_image", TOOL)
cdi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cdi)


def _record(rtype, addr16, data):
    body = bytes([len(data), (addr16 >> 8) & 0xFF, addr16 & 0xFF, rtype]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def _hex(tmp_path, segments, name="img.hex"):
    """segments: list of (address, length). Writes 16-byte data records,
    with a type-04 record whenever the upper 16 address bits change."""
    lines, upper = [], 0
    for addr, length in segments:
        for off in range(0, length, 16):
            a = addr + off
            if a >> 16 != upper:
                upper = a >> 16
                lines.append(_record(0x04, 0, upper.to_bytes(2, "big")))
            lines.append(_record(0x00, a & 0xFFFF, bytes(min(16, length - off))))
    lines.append(_record(0x01, 0, b""))
    path = tmp_path / name
    path.write_text("\n".join(lines) + "\n")
    return str(path)


def test_accepts_app_at_0x1000(tmp_path):
    assert cdi.check(_hex(tmp_path, [(0x1000, 0x1000)])) == []


def test_rejects_data_below_0x1000(tmp_path):
    problems = cdi.check(_hex(tmp_path, [(0x0000, 0x20), (0x1000, 0x100)]))
    assert len(problems) == 1
    assert "0x00000000" in problems[0]


def test_rejects_data_at_bootloader(tmp_path):
    problems = cdi.check(_hex(tmp_path, [(0x1000, 0x100), (0xE0000, 0x20)]))
    assert len(problems) == 1
    assert "0x000E0000" in problems[0]


def test_rejects_uicr(tmp_path):
    problems = cdi.check(_hex(tmp_path, [(0x1000, 0x100), (0x10001000, 0x10)]))
    assert len(problems) == 1
    assert "UICR" in problems[0]


def test_handles_extended_linear_address(tmp_path):
    # 0x2_1000 is written as type-04 upper 0x0002 plus offset 0x1000. A
    # parser that ignored type 04 would see 0x1000 and accept the image.
    problems = cdi.check(_hex(tmp_path, [(0x1000, 0x10), (0xE1000, 0x10)]))
    assert len(problems) == 1
    assert "0x000E1000" in problems[0]


def test_rejects_image_not_starting_at_0x1000(tmp_path):
    problems = cdi.check(_hex(tmp_path, [(0x2000, 0x100)]))
    assert len(problems) == 1
    assert "0x00002000" in problems[0]


def test_cli_exit_codes(tmp_path):
    good = _hex(tmp_path, [(0x1000, 0x30)], "good.hex")
    bad = _hex(tmp_path, [(0x0000, 0x10)], "bad.hex")
    ok = subprocess.run([sys.executable, str(TOOL), good],
                        capture_output=True, text=True)
    assert ok.returncode == 0
    assert ok.stdout.strip() == "OK: 0x1000-0x1030"
    ko = subprocess.run([sys.executable, str(TOOL), bad],
                        capture_output=True, text=True)
    assert ko.returncode == 1
    assert "0x00000000" in ko.stdout
