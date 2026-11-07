from kmbench import interfere


def test_jam_report_parses_firmware_text():
    # Exactly what jam.c prints when the interferer stops.
    assert interfere.jam_report("ok jam off: ch 45, 1234 packets, 19.8 % on air") == (
        45, 1234, 19.8)


def test_jam_report_ignores_other_text():
    assert interfere.jam_report("ok jam off: not running") is None
    assert interfere.jam_report("ok jam ch 45 20 %") is None
