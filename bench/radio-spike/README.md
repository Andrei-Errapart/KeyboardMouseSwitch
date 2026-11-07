# Radio bench spike

Throwaway firmware and host tools that compare radio candidates A and B
(spec risk 3). Plan: `docs/superpowers/plans/2026-11-07-radio-bench-spike.md`.

The spike uses a public bench key, never `~/.kmswitch-key`, so its images
and DFU zips are not secret.

## Toolchain

- nRF Connect SDK **v3.4.2** with its toolchain, installed by
  `nrfutil sdk-manager install v3.4.2` into `/opt/nordic/ncs` (macOS allows
  no other location).
- `nrfutil` 9.0.0 in `~/.local/bin` on the Mac mini and on anduq7, with
  `nrf5sdk-tools` 1.1.0 on both and `sdk-manager` 1.17.0 on the Mac mini.
  On anduq7, `~/.local/bin` isn't on the PATH of non-interactive SSH, so
  scripts call `~/.local/bin/nrfutil`.

`west` only finds its `build` command inside the NCS workspace, so builds
run through the toolchain launcher with `--chdir`:

```
nrfutil sdk-manager toolchain launch --ncs-version v3.4.2 \
    --chdir /opt/nordic/ncs/v3.4.2 -- \
    west build -b nrf52840dongle/nrf52840 -d ~/build/kms/radio-spike \
    "$PWD/bench/radio-spike/firmware"
```

The build directory stays on the Mac mini's own disk; the repository is on
SMB. With sysbuild the application image is
`~/build/kms/radio-spike/firmware/zephyr/zephyr.hex`.

## Image placement

Zephyr's flash region for `nrf52840dongle/nrf52840` starts at `0x1000` but
runs to the end of flash (1020 KB), past the bootloader at `0xE0000`, so the
linker would not stop an image that is too large. Check every image before
a DFU:

```
tools/check-dongle-image.py ~/build/kms/radio-spike/firmware/zephyr/zephyr.hex
```

Zephyr's `hello_world` for this board gives `OK: 0x1000-0xc848` with
NCS v3.4.2.

## Build, flash, query

From `bench/radio-spike/host`:

```
python3 -m kmbench build              # west build, placement check, DFU zip
python3 -m kmbench flash both         # or macmini / anduq7
python3 -m kmbench info both
python3 -m kmbench dfu macmini        # reboot into the bootloader (P0.19)
```

`flash` reboots a running dongle into its bootloader with `kmb dfu`, waits
for the bootloader to answer a DFU ping, runs `nrfutil nrf5sdk-tools dfu
usb-serial` (on anduq7 over SSH, from the repo's `/data/...` path) and waits
for the new firmware's Boot record with the expected serial. No button.

Checked on 2026-11-07 with both dongles:

- the firmware's USB serial matches the bootloader's (`E1380B5925A4`,
  `DA1EAB2D90C1`), as `1209:0001` "KMSwitch KMS radio bench";
- `kmb dfu` (P0.19 low) reaches the bootloader, which then lists the
  application at `0x01000`;
- the watchdog resets a hung dongle in 2 s, `kmb reboot` goes through it,
  and it runs again after that reboot;
- ModemManager on anduq7 leaves the port alone (CDC protocol 0).

## Record stream limits

While the host reads, the USB stack takes most of the CPU: the stream tops
out at about 3,000 STATS-sized records per second (≈147 KB/s), and a
thread-level producer is slowed to that rate (about 330 µs per record
instead of 32 µs). Records written from interrupts are not slowed; above
that rate they are dropped and counted in `rec_dropped`. So test traffic is
generated in interrupt context, and high-rate events are logged sparingly.

## Link encryption timing

`kmb selftest` checks AES-CCM against NIST SP 800-38C examples 1 and 2 and
the bench's own packet layout (all computed independently by
`host/tests/test_vectors.py`), the replay check and a tampered packet, and
times the operations. Measured on both dongles on 2026-11-07, for a 17-byte
body:

| Implementation | Seal | Open | Session key |
|---|---|---|---|
| PSA Crypto, CC310 driver | 240 µs | 246 µs | 251 µs |
| PSA Crypto, Oberon (software) | 250 µs | 254 µs | 172 µs |
| ECB peripheral + software CCM (used) | 94 µs | 95 µs | 12 µs |

PSA's cost is per call, not AES: one 16-byte block through
`psa_cipher_encrypt` took about 92 µs on either driver. With PSA a report
would spend about 0.5 ms in crypto, half the 1 ms over-the-air bar, so the
bench uses the spec's "ECB peripheral with software CCM" option: about
190 µs per report, within "a few hundred µs". The CCM peripheral (the
third option) would likely be faster still; not measured.

The CC310 reads its input by DMA, which can't reach flash: PSA returns
`PSA_ERROR_DATA_INVALID` for `const` buffers. Worth knowing if the product
uses PSA.
