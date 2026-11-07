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

## Channel and interference

On 2026-11-07, with the bridge on anduq7 under the table and the receiver
at the Mac mini beside the Keychron receivers, PINGs acknowledged out of
about 40 in 4 s (candidate B):

| Channel | MHz | Acknowledged | Receiver heard |
|---|---|---|---|
| 5 | 2405 | no link | — |
| 25 | 2425 | 25 of 31 | 29 |
| 50 | 2450 | barely linked | — |
| 77 | 2477 | 26 of 40 | 36 |
| 80 | 2480 | 40 of 40 | 40 |

Loss falls mostly on packets the bridge receives (ACKs, HELLOs), so the
bridge's spot is the noisier one. Channel 80 is the default until T8 picks
one. Fast ramp-up made no difference on channel 80 (38–40 of 40 either
way), so it stays on.

## RSSI scan

`python3 -m kmbench scan both --dwell 100` samples every channel on both
dongles at once. On 2026-11-07, with no bench traffic:

- anduq7's dongle (under the table) has a broadband noise floor of
  -78 to -80 dBm across the whole band, above -80 dBm 66–100% of the time
  at 2400–2445 MHz and 10–40% above that: USB 3 or PC noise at that spot.
  The Mac mini's dongle sees -83 to -84 dBm.
- At the Mac mini, strong bursts (up to -18 dBm) at 2423–2433 and
  2470–2475 MHz: the Keychron receivers beside it.
- Which channels work changes within minutes: channel 80 went from 40 of
  40 PINGs acknowledged to 1 of 15.

The bridge's reception is the weak link, and the product's bridge sits on
the hub on anduq7, in the same spot unless it goes on an extension.

## Candidate A loses reports where B doesn't

With ESB, A's bridge drops a report as delivered when the next new poll
arrives, even if every ACK that carried it was lost and the receiver gave
up on that poll. On channel 68, with 12% of polls failing all four
attempts, A lost 30 of 200 reports; of the lost ones, 22 had their
carrying poll fail at the receiver, against 1 of the 170 delivered. B, run
interleaved on the same channels, lost 1–4 of 200. Under clean conditions
A lost none.

A also learns it is active only at an idle poll: when the switch comes, a
STATUS already waits in ESB for that pipe and can't be replaced, so the
first input can wait about two idle polls (up to ~200 ms), not one.
