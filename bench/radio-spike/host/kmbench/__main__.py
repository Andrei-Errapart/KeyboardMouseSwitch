"""Command line for the radio bench: python -m kmbench <command> <dongle>.

Run from bench/radio-spike/host. Dongles: macmini, anduq7 or both.
"""
import argparse
import importlib.util
import os
import pathlib
import subprocess
import sys
import time

from . import port, records

REPO = pathlib.Path(__file__).resolve().parents[4]
FIRMWARE = REPO / "bench/radio-spike/firmware"
OUT = REPO / "bench/radio-spike/out"
NCS = "v3.4.2"
NCS_DIR = f"/opt/nordic/ncs/{NCS}"
BUILD_DIR = pathlib.Path.home() / "build/kms/radio-spike"
HEX = BUILD_DIR / "firmware/zephyr/zephyr.hex"
ZIP = OUT / "radio-spike.zip"


def _check_image(hex_path):
    spec = importlib.util.spec_from_file_location(
        "check_dongle_image", REPO / "tools/check-dongle-image.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    problems = mod.check(str(hex_path))
    if problems:
        sys.exit(f"{hex_path}: " + "; ".join(problems))


def build(_args):
    subprocess.run(["nrfutil", "sdk-manager", "toolchain", "launch",
                    "--ncs-version", NCS, "--chdir", NCS_DIR, "--",
                    "west", "build", "-b", "nrf52840dongle/nrf52840",
                    "-d", str(BUILD_DIR), str(FIRMWARE)], check=True)
    _check_image(HEX)
    make_zip()
    print(f"built {HEX} and {ZIP}")


def make_zip():
    _check_image(HEX)
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["nrfutil", "nrf5sdk-tools", "pkg", "generate",
                    "--hw-version", "52", "--sd-req", "0x00",
                    "--application-version", "1", "--application", str(HEX),
                    str(ZIP)], check=True, stdout=subprocess.DEVNULL)


def wait_boot(p, serial, timeout=10.0):
    """The next Boot record; asks with `kmb info` in case it was missed."""
    p.send("kmb info")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for rec in p.records(timeout=0.5):
            if isinstance(rec, records.Boot):
                if rec.serial != serial:
                    sys.exit(f"serial {rec.serial}, expected {serial}")
                return rec
    raise TimeoutError("no Boot record")


def describe(name, boot):
    role = {0: "none", 1: "bridge", 2: "receiver"}.get(boot.role, boot.role)
    return (f"{name} {boot.serial} fw={boot.fw} role={role} "
            f"cand={boot.cand or '-'}")


def enter_bootloader(name):
    """Reboot the application into the bootloader if it is running."""
    try:
        p = port.open_port(name)
    except (port.PortClosed, OSError):
        return
    try:
        if p.first(2.5) is not None:
            p.send("kmb dfu")
            time.sleep(0.2)
    finally:
        p.close()


def run_nrfutil_dfu(name, path):
    d = port.DONGLES[name]
    if d.host is None:
        cmd = ["nrfutil", "nrf5sdk-tools", "dfu", "usb-serial",
               "-pkg", str(ZIP), "-p", path]
    else:
        remote_zip = f"{port.REPO_ON_ANDUQ7}/{ZIP.relative_to(REPO)}"
        cmd = ["ssh", "-o", "BatchMode=yes", d.host,
               f"{port.REMOTE_NRFUTIL} nrf5sdk-tools dfu usb-serial "
               f"-pkg {remote_zip} -p {path}"]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit(f"{name}: nrfutil DFU failed:\n{done.stdout}{done.stderr}")


def flash(name):
    d = port.DONGLES[name]
    enter_bootloader(name)
    path = port.wait_for_bootloader(name, timeout=10)
    print(f"{name}: bootloader at {path}, flashing")
    run_nrfutil_dfu(name, path)
    p = port.reopen(name, timeout=15)
    try:
        print(describe(name, wait_boot(p, d.serial)))
    finally:
        p.close()


def info(name):
    p = port.open_port(name)
    try:
        print(describe(name, wait_boot(p, port.DONGLES[name].serial, 5)))
    finally:
        p.close()


def selftest(name):
    p = port.open_port(name)
    try:
        p.send("kmb selftest")
        end = time.monotonic() + 5
        while time.monotonic() < end:
            for rec in p.records(timeout=0.5):
                if isinstance(rec, records.SelfTest):
                    ok = {k: "ok" if getattr(rec, f"{k}_ok") else "FAIL"
                          for k in ("nist", "layout", "replay", "tamper")}
                    print(f"{name} " + " ".join(f"{k}={v}" for k, v in ok.items())
                          + f" seal={rec.enc_us}us open={rec.dec_us}us"
                          f" session_key={rec.sk_us}us")
                    return rec
        raise TimeoutError(f"{name}: no SELFTEST record")
    finally:
        p.close()


def dfu(name):
    enter_bootloader(name)
    print(f"{name}: bootloader at {port.wait_for_bootloader(name)}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="kmbench")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    for cmd in ("info", "flash", "dfu", "selftest"):
        sub.add_parser(cmd).add_argument(
            "dongle", choices=[*port.DONGLES, "both"])
    args = ap.parse_args(argv)
    if args.cmd == "build":
        return build(args)
    if args.cmd == "flash":
        make_zip()
    names = list(port.DONGLES) if args.dongle == "both" else [args.dongle]
    for name in names:
        {"info": info, "flash": flash, "dfu": dfu,
         "selftest": selftest}[args.cmd](name)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    main()
