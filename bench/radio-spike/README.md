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
