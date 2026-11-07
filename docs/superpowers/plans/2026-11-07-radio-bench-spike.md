# Radio Bench Spike (Candidates A and B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure radio candidates A and B on the two dongles we have, with
encryption on. Pick one, and record the choice, the channel and the ESB
parameters in the spec (risk 3). Along the way, settle the dongle checks from
risk 6 that need no key (image placement, reboot into the bootloader through
P0.19) and the USB serial identity.

**Architecture:** One throwaway nRF Connect SDK application, flashed to both
dongles.
- Role (bridge or receiver) and candidate (A or B) are chosen at run time
  with `kmb` text commands over CDC, so changing either needs no reflash.
- Each dongle timestamps every radio ADDRESS event in hardware on a 1 MHz
  timer and streams binary event records over CDC.
- A Python tool on the Mac mini drives both dongles, reaching anduq7's
  through `ssh anduq7 socat`. It merges the two logs by fitting the offset
  between the two clocks from packets that got through on the first try,
  then computes latency, loss, airtime, switch delay and discovery time.

**Tech Stack:**
- nRF Connect SDK v3.4.2, with Zephyr, ESB and the new USB device stack.
  Pinned: v3.3.5 came out two days later on the older line, so "the newest
  release" is ambiguous. The ESB details below are from v3.4.2's
  `subsys/esb/esb.c`.
- PSA Crypto on the CC310: AES-128-CCM with a 4-byte MIC.
- Python 3, with pytest and `cryptography` through `uv run --with`.
- `nrfutil`, with `sdk-manager` and `nrf5sdk-tools`.

**Spec:** `docs/superpowers/specs/2026-11-04-keyboard-mouse-switch-design.md`:
"Radio link", "Dongle firmware", "Reflashing", and risks 3 and 6.

## Global Constraints

- **Board and bootloader.** Board `nrf52840dongle/nrf52840`; the factory Open
  DFU bootloader stays. The image starts at `0x1000` and ends below `0xE0000`,
  where the bootloader starts (per `tools/dongle-info.py`).
- **No real key.** The bench's pre-shared key is the public constant
  `"KMS bench key 01"` in the source, commented as bench-only. The spike never
  reads `~/.kmswitch-key`, so its hex files and DFU zips aren't secret.
- **Dongles.**

  | Name | Serial | Port | Default role |
  |---|---|---|---|
  | `macmini` | `E1380B5925A4` | `/dev/cu.usbmodemE1380B5925A41` | receiver |
  | `anduq7` | `DA1EAB2D90C1` | `/dev/serial/by-id/usb-*_DA1EAB2D90C1-if00` | bridge |

  The Mac mini's dongle is on its 4-port USB 2.0 hub, beside the Keychron
  Link-KM and the Keychron Receiver. anduq7's is under the table.
- **USB identity.**
  - VID:PID `1209:0001`; manufacturer `KMSwitch`; product `KMS radio bench`.
  - Serial: `"%04X%08X"` of `(uint16_t)NRF_FICR->DEVICEADDR[1] | 0xC000` and
    `NRF_FICR->DEVICEADDR[0]`. This is how the bootloader makes its serial
    (nRF5 SDK `app_usbd_serial_num.c`).
- **Commands.** Host commands are text lines starting with `kmb `. Anything
  else is ignored, such as ModemManager probes and stray bytes.
- **Radio defaults.** All can be changed with `kmb set`:
  - ESB at 2 Mbit/s, dynamic payload length, 16-bit CRC, fast ramp-up;
  - 0 dBm, channel 50 (2450 MHz);
  - retransmit delay 435 µs, the lowest ESB accepts: below it, `esb_init`
    and `esb_set_retransmit_delay` return `-EINVAL`. Each retry therefore
    costs at least 435 µs;
  - retries: reports 3, polls 3, pings 1, hellos 0.
- **Addresses.** Discovery uses pipe 0 (base0 `0x4B4D5344`, prefix `0xC1`).
  Receivers use pipes 1–7 (base1 `0x4B4D5352`, prefixes `0xC2`–`0xC8`). The
  bridge assigns a receiver's pipe at discovery.
- **Timing, from the spec:**
  - B: refresh after 25 ms with nothing sent to the active receiver; pings
    every 100 ms, to linked receivers only;
  - A: the active receiver polls every 500 µs, idle receivers every 100 ms;
  - unlinked receivers send a hello every 20 ms, plus a random 0–10 ms;
  - a link counts as lost after 1 s without a valid packet;
  - discovery target: about 1 s.
- **Packet sizes.** A report is 25 bytes on air: a 4-byte counter in the
  clear, a 17-byte encrypted body and a 4-byte MIC. This is the spec's "about
  25 bytes". Polls, pings and status packets are 16 bytes: an 8-byte body.
- **Where files go.**
  - The build directory is on the Mac mini's own disk,
    `~/build/kms/radio-spike`, because the repo is on SMB.
  - DFU zips go to `bench/radio-spike/out/`, which is gitignored. anduq7 sees
    it at `/data/workspace/macmini/KeyboardMouseSwitch/bench/radio-spike/out/`.
  - Raw logs go to `bench/radio-spike/results/`, also gitignored. The summary
    goes to `docs/bench/2026-11-radio-ab.md` and is committed.
- **Commits.** The owner commits on request: ask before the first commit of
  the run, and "commit as you go" covers the rest. The repo is public, so
  nothing from `~` goes in.
- **Host tests:**
  `uv run --with pytest --with cryptography python -m pytest bench/radio-spike/host/tests tools/tests -q`

## Review Focus

1. **A clock fit that goes wrong quietly.** Too few first-try packets, a
   reboot mid-run (the timer restarts at 0), or residuals over 20 µs must
   stop the analysis with a message, never print latencies. Tested in Task 2.
2. **Records lost on CDC look like radio loss.** The firmware counts every
   record it couldn't queue and reports the count in STATS. The count runs
   from boot, so a run is judged by how much it rose between the run's
   first STATS and a STATS requested after its traffic stopped: any rise
   makes the run invalid, and drops from before the run don't. For this to
   work the first STATS a run reads must be current, so the once-a-second
   STATS is written only while the port is open (Task 3). Tested in Task 2
   (analysis) and Task 3 (firmware counter under `kmb flood`).
3. **The 32-bit microsecond timer wraps every 71.6 minutes,** counted from
   boot, so a long session crosses it. A record's address stamps can be a
   little older than its `t`, so they can lie on the other side of a wrap.
   Tested in Task 2 (unwrap).
4. **A port that vanishes mid-run,** through a dongle reboot, a USB reset or
   a dropped SSH session, ends the run as aborted within 2 s instead of
   hanging. A stalled SSH session closes nothing, so 2 s without a STATS
   record also aborts the run. Reboots the run asks for itself (`kmb hang`,
   `kmb reboot`, T7) are different: the harness reopens the port and
   expects one Boot. Tested in Task 3 (fake port).
5. **A retransmission isn't a duplicate.** An ESB retransmission after a lost
   ACK is not a duplicate; the same report ID reaching the receiver's
   application twice is. Tested in Task 2.

## Owner actions

- Task 1 needs, once, on the Mac mini:
  `! sudo mkdir -p /opt/nordic && sudo chown $USER /opt/nordic`
  (NCS on macOS installs only there).
- **Task 8, the reliability and channel runs (about 45 min):**
  - keep the Keychron keyboard and mouse in use;
  - make Wi-Fi busy, for example a large download on the MacBook Pro over
    Wi-Fi;
  - say whether the Keychron mouse stutters while candidate A runs.
- If a flash ever leaves a dongle unresponsive, press its side reset button.
  For anduq7's dongle, that means reaching under the table.

---

## File Structure

```
tools/check-dongle-image.py      image placement check (risk 6); the real dongle build reuses it
tools/tests/test_check_dongle_image.py
bench/radio-spike/
  README.md                      build, flash, run; the NCS version used
  .gitignore                     out/, results/
  firmware/
    CMakeLists.txt  prj.conf
    src/main.c                   init, command loop, watchdog, STATS every second
    src/cmd.c  cmd.h             `kmb` parser and dispatch
    src/rec.c  rec.h             record ring (ISR-safe), SLIP encoder, CDC writer, drop counter
    src/stamp.c  stamp.h         TIMER3 at 1 MHz; ADDRESS capture through PPI + EGU3
    src/ident.c  ident.h         serial from DEVICEADDR, USB descriptors, P0.19 reboot
    src/link.c  link.h           PSA CCM, session key, nonces, replay check, self-test
    src/pkt.h                    packet kinds and body layouts
    src/radio.c  radio.h         ESB setup per mode, pipes, mode-switch timing
    src/traffic.c  traffic.h     test patterns, per-receiver queues, motion merge
    src/cand_b.c  src/cand_a.c   the two candidates, bridge and receiver sides
    src/scan.c                   RSSI scan of channels 0–80
  host/
    kmbench/slip.py  records.py  clockfit.py  metrics.py   pure, unit-tested
    kmbench/port.py              local and SSH ports, flashing
    kmbench/runs.py  report.py   test definitions T0–T8, pass/fail, markdown
    kmbench/__main__.py          CLI: info, flash, dfu, selftest, smoke, run, report
    tests/test_*.py
docs/bench/2026-11-radio-ab.md   results and decision
```

`tools/dongle-info.py` keeps its own SLIP code: it must stay a single file
that can be copied anywhere.

## Record format (shared by Tasks 2, 3, 5, 6, 7)

One record per SLIP frame, little-endian, byte 0 is the type. `t` is the
sender's 1 MHz clock at the moment the record was made.

| Type | Name | Fields after the type byte |
|---|---|---|
| `0x01` | BOOT | u32 t, u32 boot_id (random per boot), u8 role (0 none, 1 bridge, 2 receiver), u8 cand (0, `'A'`, `'B'`), char serial[12], char fw[16] |
| `0x02` | TEXT | UTF-8 text: command replies and errors |
| `0x03` | STATS | u32 t, u32 addr_tx, u32 addr_rx, u32 rec_dropped, u32 stamp_lost, u32 mic_fail, u32 replay_reject, u32 merged, u32 queue_dropped, u32 mode_switches, u32 mode_switch_us_total, u16 mode_switch_us_max |
| `0x10` | QUEUED | u32 t, u32 id, u8 pipe |
| `0x11` | TXDONE | u32 t, u32 id, u8 pipe, u8 kind, u8 attempts, u8 ok, u8 len, u8 ack_len, u32 t_tx_addr, u32 t_rx_addr |
| `0x12` | RXPKT | u32 t, u32 id, u8 pipe, u8 kind, u8 len, i8 rssi, u32 t_rx_addr |
| `0x13` | LINK | u32 t, u8 event (2 hello heard, 3 established, 4 lost, 5 active, 6 inactive), u8 pipe, u8 peer[6] |
| `0x14` | SCAN | u8 channel, i8 rssi_min, i8 rssi_median, i8 rssi_max, u16 busy_permille |
| `0x15` | SELFTEST | u8 nist_ok, u8 layout_ok, u8 replay_ok, u8 tamper_ok, u16 enc_us, u16 dec_us, u16 sk_us |
| `0x16` | MERGED | u32 t, u32 id, u32 into, u8 pipe |

**What the fields mean:**
- **IDs.** Every dongle numbers every packet it sends from 1 upward per
  boot, so a (kind, id) pair names one packet from one sender.
- **ADDRESS stamps.** `t_tx_addr` and `t_rx_addr` are the timer values at
  the most recent radio ADDRESS event sent and received, respectively, when
  the record was made.
- **TXDONE:** written by the side that transmits (PTX) when ESB reports
  success or failure.
- **RXPKT:** written for every packet the application accepts, including
  ACK payloads.
- **QUEUED:** written by the bridge when the traffic generator creates a
  test report.
- **MERGED:** written by the bridge when the motion report `id` merges into
  the pending report `into`. It reaches the receiver as `into`.
- **stamp_lost** counts ADDRESS events whose direction couldn't be told
  (see Task 3). Like `rec_dropped`, any rise during a run makes it invalid.

**Clock pairs.** A clock pair is a TXDONE with `attempts == 1` and `ok == 1`
on one dongle, matched to the RXPKT with the same kind and id on the other.
It pairs the sender's `t_tx_addr` with the receiver's `t_rx_addr`: the same
instant on two clocks.

## Packets (Tasks 4–6)

- **Session packets:** `counter u32` in the clear, then CCM(body), then a
  4-byte MIC.
  - Nonce: `counter LE32 ‖ dir ‖ 8 zero bytes`, where dir 0 means bridge →
    receiver and dir 1 means receiver → bridge.
  - Body: `kind u8, flags u8 (bit0 active, bit1 more pending), id u32, data`.
  - REPORT data is 11 bytes. PING, POLL and STATUS data is 2 bytes.
- **Discovery packets** are sealed under the pre-shared key, with nonce
  `4 zero bytes ‖ dir ‖ salt`:
  - HELLO body: `kind, version, rx_addr[6], salt_r[8]`;
  - WELCOME body: `kind, pipe, salt_b[8]`.
- **Kinds:** HELLO 1, WELCOME 2, REPORT 3, PING 4, STATUS 5, POLL 6.
- **Session key:** SK = AES-128-ECB(PSK, salt_b ‖ salt_r).
- **Replay check:** counters start at 0 each session (spec). Each side
  starts a session with no counter accepted, accepts the first packet that
  verifies, and from then on rejects a counter not above the last accepted
  one. Starting from "last accepted = 0" instead would reject counter 0.
- **Handshake, the same for A and B:**
  1. The receiver sends a HELLO on pipe 0.
  2. The bridge always has a WELCOME preloaded as pipe 0's ACK payload, with
     a fresh salt and the next free pipe. It loads a new one after each
     HELLO it hears.
  3. Both derive SK, and the first session packet that verifies on each side
     establishes the link.
- **Pipes:**
  - The pipe a WELCOME offers is held for the HELLO that took it for 100 ms.
    If no session is established on it by then, it is free again, so a lost
    ACK doesn't use up a pipe.
  - The next WELCOME offers the next pipe that is neither linked nor held.
  - When a session is established for an `rx_addr` that already has a pipe,
    for example after the receiver rebooted (T7), the new session replaces
    the old one and the old pipe is freed. Until then the old link runs on.

---

### Task 1: Toolchain and image placement check

**Files:**
- Create: `tools/check-dongle-image.py`, `tools/tests/test_check_dongle_image.py`, `bench/radio-spike/README.md`

**Interfaces:**
- Produces: `check(hex_path: str) -> list[str]`, which returns the problems
  found (empty means fine). The CLI `tools/check-dongle-image.py <hex>`
  prints `OK: 0x1000-0x<end>` and exits 0, or prints each problem and
  exits 1.

- [ ] **Step 1: Owner creates `/opt/nordic`** (see Owner actions). Check: `ls -ld /opt/nordic` shows `andrei`.
- [ ] **Step 2: Install nrfutil on both machines.**
  - Download from `https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/<target>/nrfutil`
    to `~/.local/bin/nrfutil` and `chmod +x` it. The target is
    `universal-apple-darwin` on the Mac mini and `x86_64-unknown-linux-gnu`
    on anduq7.
  - Then run `nrfutil install sdk-manager nrf5sdk-tools` on the Mac mini and
    `nrfutil install nrf5sdk-tools` on anduq7.
  - Check: `nrfutil nrf5sdk-tools --help` works on both.
- [ ] **Step 3: Install NCS.**
  - Run `nrfutil sdk-manager install v3.4.2` (pinned, see Tech Stack).
    Write the version into `bench/radio-spike/README.md`.
  - Check: `df -h ~` still shows at least 5 GB free (17 GB before).
- [ ] **Step 4: Write the failing tests.** Each builds a synthetic Intel HEX
  in `tmp_path`:
  - `test_accepts_app_at_0x1000`: data at 0x1000–0x1FFF gives `[]`.
  - `test_rejects_data_below_0x1000`: data at 0x0000 gives one problem,
    containing `0x00000000`.
  - `test_rejects_data_at_bootloader`: data at 0xE0000 gives one problem,
    containing `0x000E0000`.
  - `test_rejects_uicr`: data at 0x10001000 gives one problem, containing
    `UICR`.
  - `test_handles_extended_linear_address`: type-04 records are honoured.
- [ ] **Step 5: Run the tests to verify they fail.** Run: the host test
  command. Expected: FAIL (module missing).
- [ ] **Step 6: Implement `check()`.** Parse record types 00, 01, 02 and 04;
  ignore 03 and 05. Allowed range: `0x1000 <= addr < 0xE0000`.
- [ ] **Step 7: Run the tests to verify they pass.** Expected: PASS.
- [ ] **Step 8: Check placement with a stock sample.**
  - Run: `nrfutil sdk-manager toolchain launch --ncs-version v3.4.2 -- west build -b nrf52840dongle/nrf52840 -d ~/build/kms/hello /opt/nordic/ncs/v3.4.2/zephyr/samples/hello_world`,
    then `tools/check-dongle-image.py ~/build/kms/hello/hello_world/zephyr/zephyr.hex`.
  - Expected: `OK: 0x1000-…`.
  - If it says otherwise, sysbuild's Partition Manager moved the image. Add
    `SB_CONFIG_PARTITION_MANAGER=n` (or a `pm_static.yml`) and repeat
    before any DFU.
- [ ] **Step 9: Commit** `tools/check-dongle-image.py tools/tests bench/radio-spike/README.md`, message "Add dongle image placement check".

### Task 2: Host analysis library

**Files:**
- Create: `bench/radio-spike/.gitignore` (containing `out/` and `results/`)
- Create, in `bench/radio-spike/host/kmbench/`: `__init__.py`, `slip.py`,
  `records.py`, `clockfit.py`, `metrics.py`
- Create: `bench/radio-spike/host/tests/test_slip.py`, `test_records.py`,
  `test_clockfit.py`, `test_metrics.py`

**Interfaces:**
- **slip.py:** `encode(data: bytes) -> bytes` and
  `Decoder().feed(data: bytes) -> list[bytes]`.
- **records.py:** frozen dataclasses `Boot`, `Text`, `Stats`, `Queued`,
  `TxDone`, `RxPkt`, `Link`, `Scan`, `SelfTest`, `Merged`, with the field
  names from the record table. Plus:
  - `parse(frame: bytes) -> Record`, which raises `ValueError` on an unknown
    type or a wrong length;
  - `Unwrapper().unwrap(t: int) -> int`, which returns `t` in the 2³²
    window closest to the largest value seen so far, then remembers the
    larger of the two. It takes a record's `t` and also its address stamps,
    which can be a little older. This holds while successive records are
    less than 2³¹ µs (35 min) apart, which STATS every second ensures.
    `reset()` is called on every Boot.
- **clockfit.py:**
  - `pairs(bridge: list[Record], receiver: list[Record]) -> list[tuple[int, int]]`
    returns (t_bridge, t_receiver) pairs, following "Clock pairs" above;
  - `fit(pairs) -> Fit(offset_us: float, drift_ppm: float, resid_p99_us: float, n: int)`
    fits `t_receiver = t_bridge * (1 + drift_ppm / 1e6) + offset_us`. It
    raises `ClockFitError` if `n < 20` or `resid_p99_us > 20`;
  - `Fit.to_bridge(t_receiver: int) -> float`.
- **metrics.py:**
  - `latency(bridge, receiver, fit) -> Latency(samples_us: list[float], p50: float, p99: float, max: float, delivered: int, lost: int, dups: int, merged: int)`.
    It pairs each Queued with the receiver's first RxPkt of kind REPORT with
    the same id. A Queued id that has a Merged record is looked up as its
    `into` id, following chains, and counts as merged, not lost; its latency
    runs from its own Queued time;
  - `airtime(records, seconds: float) -> float`, the fraction of time a
    packet is on air, which is what other 2.4 GHz users see. From every
    TxDone: `attempts * air(len) + (air(ack_len) if ok else 0)`, with
    `air(n) = 4 * (2 + 5 + n + 2) + 4.5` µs (2-byte preamble, 5-byte
    address, 9-bit packet control field and 16-bit CRC at 2 Mbit/s);
  - `transaction_us(records) -> list[float]`, the length of each first-try
    transaction including ramp-up and turnaround, which is what the spec's
    "300–450 µs" counts. For each TxDone with `attempts == 1` and `ok == 1`:
    `40 + (t_rx_addr - t_tx_addr) + air(ack_len)`. 40 µs is the fast
    ramp-up; the 28 µs of preamble and address before each ADDRESS event
    cancel out;
  - `switch_delays(bridge, receiver, fit) -> list[float]`, in ms: from a
    bridge Link event 5 (active) to the first report the receiver gets
    after it;
  - `discovery_times(records) -> list[float]`, in ms: each Boot's `t` to the
    next Link event 3 on the same dongle;
  - `check_run(records, allow_reboot=False) -> None`, which raises
    `RunInvalid`:
    - when `rec_dropped` or `stamp_lost` rose between the run's first and
      last Stats (per boot, when reboots are allowed);
    - when no Stats follows the run's last other record. Every run therefore
      ends with `kmb stats` on both dongles after its traffic stops;
    - on a Boot after the first record, unless `allow_reboot`.

- [ ] **Step 1: Write the failing tests:**
  - `test_slip_roundtrip`: frames containing `0xC0` and `0xDB` survive, also
    when split across `feed` calls.
  - `test_parse_txdone`: hand-packed bytes give the right fields.
  - `test_parse_rejects_short_frame`: raises `ValueError`.
  - `test_parse_merged`: a hand-packed `0x16` frame gives a `Merged` with
    the right `id`, `into` and `pipe`.
  - `test_unwrap_across_wrap`: `[0xFFFFFF00, 0x100]` gives
    `[4294967040, 4294967552]`.
  - `test_unwrap_resets_on_boot`.
  - `test_unwrap_stamp_before_wrap`: after `[0xFFFFFF00, 0x100]`, the stamp
    `0xFFFFFFF0` gives `4294967280`.
  - `test_fit_recovers_offset_and_drift`: 500 synthetic pairs, offset
    123456 µs, drift 25 ppm, noise ±3 µs. Expect the offset within 2 µs and
    the drift within 1 ppm.
  - `test_fit_refuses_few_pairs`: 19 pairs raise `ClockFitError`.
  - `test_fit_refuses_noise`: ±50 µs noise raises `ClockFitError`.
  - `test_pairs_first_attempt_only`: a TxDone with `attempts=2` gives no
    pair.
  - `test_latency_basic`: queued at bridge t=1000, received at receiver
    t=offset+1350 gives `samples_us == [350.0]`.
  - `test_latency_counts_lost`: a Queued without an RxPkt gives `lost == 1`.
  - `test_latency_dup_counts_once`: the same id received twice gives
    `dups == 1`, and the latency comes from the first.
  - `test_retransmit_is_not_dup`: a TxDone with `attempts=3` and one RxPkt
    gives `dups == 0`.
  - `test_latency_merged_is_not_lost`: id 5 merged into id 4, and only id 4
    received, gives `lost == 0`, `merged == 1` and two samples.
  - `test_airtime_reports`: 1000 TxDone records (len 25, ack_len 16,
    1 attempt, ok) in 1 s give `1000 * (140.5 + 104.5) / 1e6`.
  - `test_transaction_time`: `t_tx_addr` 1000, `t_rx_addr` 1250 and
    `ack_len` 16 give 394.5.
  - `test_switch_delay` and `test_discovery_time`, on synthetic records.
  - `test_run_invalid_on_dropped_records`: `rec_dropped` rises from 0 to 1.
  - `test_run_valid_with_earlier_drops`: `rec_dropped` is 5 at the first and
    the last Stats.
  - `test_run_invalid_without_final_stats`, `test_run_invalid_on_lost_stamps`
    and `test_run_invalid_on_reboot`.
- [ ] **Step 2: Run the tests to verify they fail.** Expected: FAIL (modules missing).
- [ ] **Step 3: Implement** `slip.py`, `records.py` (with `struct`),
  `clockfit.py` (least squares on the pairs, then the residuals' 99th
  percentile) and `metrics.py`.
- [ ] **Step 4: Run the tests to verify they pass.** Expected: PASS.
- [ ] **Step 5: Commit**, message "Add radio bench analysis library".

### Task 3: Firmware skeleton, ports and flashing

**Files:**
- Create: `bench/radio-spike/firmware/{CMakeLists.txt,prj.conf}`, `src/{main.c,cmd.c,cmd.h,rec.c,rec.h,stamp.c,stamp.h,ident.c,ident.h}`
- Create: `bench/radio-spike/host/kmbench/port.py`, `kmbench/__main__.py`, `tests/test_port.py`

**Interfaces:**
- **Firmware:**
  - `uint32_t stamp_now(void)`;
  - `uint32_t stamp_last_tx_addr(void)` and `uint32_t stamp_last_rx_addr(void)`;
  - `void stamp_counts(uint32_t *tx, uint32_t *rx)`;
  - `bool rec_put(const void *rec, size_t len)`. It can be called from an
    ISR. When the ring is full it returns false and counts the record in
    `rec_dropped`;
  - `void ident_serial(char out[13])`;
  - `void ident_reboot_to_bootloader(void)`, which drives P0.19 low.
- **Commands:** `kmb info`, `kmb dfu`, `kmb reboot`, `kmb stats`, `kmb hang`
  (stops feeding the watchdog) and `kmb flood <n>` (writes n STATS records
  as fast as it can). `kmb reboot` also restarts through the watchdog (see
  "Firmware details").
- **Host:**
  - `open_port(name: str) -> Port`. `macmini` opens the local port;
    `anduq7` runs
    `ssh -T -o ServerAliveInterval=1 -o ServerAliveCountMax=2 anduq7 'p=$(ls /dev/serial/by-id/usb-*_DA1EAB2D90C1-if00) && exec socat - "$p",raw,echo=0'`;
  - `Port.send(cmd: str)`, and `Port.records(timeout: float) -> Iterator[Record]`,
    which raises `PortClosed` on end of file and `PortStalled` when no STATS
    has arrived for 2 s;
  - `reopen(name: str, timeout=10) -> Port`, for reboots the run asks for. It
    retries `open_port` until the port is back. On anduq7 the by-id link is
    missing meanwhile, so the SSH command fails and is retried;
  - `wait_for_bootloader(name, timeout=10) -> str`, which returns the port
    once it answers the DFU ping (`09 5a` gives `60 09 01 5a`);
  - CLI: `python -m kmbench info|flash|dfu <macmini|anduq7|both>`.
- **The flash procedure:**
  1. If the application answers, send `kmb dfu`.
  2. Run `wait_for_bootloader`.
  3. Run `nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x00 --application-version 1 --application <hex> bench/radio-spike/out/radio-spike.zip`.
  4. Run `nrfutil nrf5sdk-tools dfu usb-serial -pkg <zip> -p <port>`,
     locally or through `ssh anduq7` with the `/data/...` path.
  5. Wait up to 10 s for a Boot record with the expected serial.
- **Firmware details:**
  - Stamp hardware: TIMER3 at 1 MHz, free-running. A PPI channel from RADIO
    `EVENTS_ADDRESS` triggers TIMER3 `CAPTURE[1]`, and its fork triggers
    EGU3. The EGU3 ISR files the captured value as TX or RX according to
    `RADIO->STATE` (11 = Tx, 3 = Rx). In any other state it ran too late to
    tell, so it discards the value and counts it in `stamp_lost`. Its IRQ
    priority must be numerically lower than both of ESB's IRQ priorities
    (1 and 2 by default), so a stamp is in place before ESB's callback runs;
    0 does it.
  - Allocate PPI channels through nrfx gppi.
  - Hold the HFXO on from boot.
  - Use the new USB device stack, with the board's CDC console backend off.
    The descriptors are set at run time from `ident_serial()`.
  - The CDC writer sends only while the host holds DTR, that is, while the
    port is open. Until then records wait in the ring, so the Boot record
    and the discovery records after a reboot survive until the harness
    reopens the port. The ring holds 8 KB, enough for the first 2 s after
    boot, HELLO TxDones included.
  - The once-a-second STATS is written only while DTR is held. Otherwise,
    after the port has been closed for a few minutes, the ring would hold
    old STATS from before drops that happened while it was full. A run
    would read one of those first, see `rec_dropped` rise, and count itself
    invalid. Gated this way, the first STATS a run reads is current.
  - No console or logging on the CDC port.
  - Watchdog timeout: 2 s, fed by the main loop.
    - A soft reset doesn't stop the nRF52840's watchdog, and a running
      watchdog can't be set up again. So `kmb reboot` stops feeding it and
      lets it reset the dongle, which does stop it.
    - Zephyr's fatal error handler still restarts through a soft reset. If
      the watchdog is already running at boot, the firmware skips setting
      it up and feeds it through `NRF_WDT->RR[0]`.
  - The `fw` string comes from `git describe --always --dirty`, at build
    time.

- [ ] **Step 1: Write the failing host tests:**
  - `test_port_closed_on_eof`: a fake command that exits raises `PortClosed`
    within 2 s.
  - `test_port_stalled_without_stats`: a fake command that stays open but
    sends nothing raises `PortStalled` within 3 s.
  - `test_reopen_after_reboot`: a fake command that fails twice and then
    works gives a working port.
  - `test_wait_for_bootloader_detects_ping`: a fake port answering
    `c0`-framed `60 09 01 5a`.
  - `test_dongle_table`: the names map to the serials and port patterns in
    Global Constraints.
- [ ] **Step 2: Run them to verify they fail. Then implement `port.py` and
  the CLI, and run them to verify they pass.**
- [ ] **Step 3: Write the firmware.** Build it with
  `west build -b nrf52840dongle/nrf52840 -d ~/build/kms/radio-spike bench/radio-spike/firmware`,
  inside the toolchain launch. Run `tools/check-dongle-image.py` on
  `~/build/kms/radio-spike/firmware/zephyr/zephyr.hex`. Expected: `OK`.
- [ ] **Step 4: First flash.** Both dongles are still in their bootloaders.
  Run `python -m kmbench flash both`, then `python -m kmbench info both`.
  Expected: `macmini E1380B5925A4` and `anduq7 DA1EAB2D90C1`, with the same
  `fw`.
- [ ] **Step 5: Identity check.**
  - Mac mini: `ioreg -p IOUSB -l -w0 | grep -A12 'KMS radio bench'` shows
    serial `E1380B5925A4`, idVendor 4617 and idProduct 1.
  - anduq7: `ls /dev/serial/by-id/` shows
    `usb-KMSwitch_KMS_radio_bench_DA1EAB2D90C1-if00`.
  - This settles the spec's open identity check.
- [ ] **Step 6: Reboot through P0.19 (risk 6).** Run
  `python -m kmbench dfu macmini`. Expected: within 10 s,
  `tools/dongle-info.py /dev/cu.usbmodemE1380B5925A41` prints
  `revision: 2`. Then `python -m kmbench flash macmini` brings the
  application back. Repeat for `anduq7`, running `dongle-info.py` through
  `ssh anduq7 python3 /data/workspace/macmini/KeyboardMouseSwitch/tools/dongle-info.py <port>`.
- [ ] **Step 7: Watchdog, reboot and record drops.**
  - `kmb hang` on macmini: the harness reopens the port, and a new Boot
    record arrives within 5 s.
  - `kmb reboot` on macmini: the same, and no further Boot follows within
    the next 5 s, so the restarted firmware feeds the watchdog.
  - Then `kmb hang`: a new Boot arrives within 5 s, so the watchdog is
    running after the reboot. The previous check alone passes even when
    there is no watchdog at all.
  - `kmb flood 100000`, then `kmb stats`: STATS reports `rec_dropped`, and
    `check_run` flags the run as invalid. A run started afterwards, without
    a reboot, is valid.
- [ ] **Step 8: ModemManager.** anduq7 runs ModemManager 1.25 with its
  default strict filter, which probes a `ttyACM` port only if its interface
  advertises AT commands (CDC protocol 1–6). Zephyr's CDC ACM advertises 0,
  like the Nordic bootloader (`02/02/00` on anduq7), so no probing is
  expected. Run
  `ssh anduq7 journalctl -u ModemManager --since -15min | grep -i -E 'ttyACM|1209'`.
  Expected: no probing. If it probes, anduhome gets an ignore rule for
  `1209:0001` (`ENV{ID_MM_DEVICE_IGNORE}="1"`); it has none yet.
- [ ] **Step 9: Commit**, message "Add radio bench firmware skeleton and host ports".

### Task 4: Link encryption and self-test

**Files:**
- Create: `bench/radio-spike/firmware/src/{link.c,link.h,pkt.h}`, `bench/radio-spike/host/tests/test_vectors.py`
- Modify: `bench/radio-spike/firmware/src/cmd.c` (`kmb selftest`), `bench/radio-spike/host/kmbench/__main__.py` (`selftest`)

**Interfaces:**
- `int link_session_key(const uint8_t salt_b[8], const uint8_t salt_r[8], psa_key_id_t *sk)`
- `int link_seal(psa_key_id_t key, uint32_t counter, uint8_t dir, const uint8_t *body, size_t len, uint8_t *out)`:
  writes `counter ‖ ciphertext ‖ MIC` and returns `len + 8`.
- `int link_open(psa_key_id_t key, uint8_t dir, struct replay *rp, const uint8_t *in, size_t len, uint8_t *body)`:
  returns the body length, `-EBADMSG` when the MIC fails, or `-EALREADY` for
  a replay. `struct replay { bool any; uint32_t last; }` starts each session
  zeroed, that is, with no counter accepted (see "Replay check").
- `int link_seal_discovery(...)` and `int link_open_discovery(...)`: the
  same, under the pre-shared key with the salt nonce.
- Algorithm: `PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4)`, 13-byte nonce.

- [ ] **Step 1: Write `test_vectors.py`.**
  - It recomputes, with `cryptography`, the constants the firmware
    self-test uses, and asserts:
    - SP 800-38C example 1 (K `404142…4f`, N `10…16`, A `00…07`,
      P `20212223`) gives `7162015b4dac255d`;
    - PSK `"KMS bench key 01"`, salt_b `10…17` and salt_r `20…27` give SK
      `b483529cc31c16b598df0b30135f3083`;
    - that SK, with nonce `05000000 01 0000000000000000` and plaintext
      `30…40` (17 bytes), gives ciphertext and MIC
      `5564a62093c965a7aa7f9aac0fde39e6bf0ff053fb`.
  - Run it. Expected: PASS. This pins the constants the firmware copies.
- [ ] **Step 2: Implement `link.c`.** `kmb selftest` checks:
  - both vectors (`nist_ok`, `layout_ok`);
  - `replay_ok`: a fresh session accepts its first packet, sealed with
    counter 0, and opening the same packet again returns `-EALREADY`. Both
    must hold;
  - `tamper_ok`: a flipped ciphertext bit returns `-EBADMSG`;
  - it then times seal and open of a 17-byte body and SK derivation, and
    writes a SELFTEST record.
- [ ] **Step 3: Build, check the image, and flash both.** Then run
  `python -m kmbench selftest both`. Expected: all four `ok`, plus the times.
  Note the times in the README; the spec's budget allows "a few hundred µs"
  for processing.
- [ ] **Step 4: Commit**, message "Add bench link encryption with self-test".

### Task 5: Radio layer, traffic and candidate B

**Files:**
- Create: `bench/radio-spike/firmware/src/{radio.c,radio.h,traffic.c,traffic.h,cand_b.c}`, `bench/radio-spike/host/kmbench/runs.py`
- Modify: `cmd.c`, `prj.conf` (`CONFIG_ESB=y`), `__main__.py` (`smoke`)

**Interfaces:**
- **`radio.h`:**
  - `int radio_mode(enum radio_mode m)`, with `RADIO_PTX` or `RADIO_PRX`:
    `esb_disable` + `esb_init` + addresses + settings, timed into STATS;
  - `int radio_send(uint8_t pipe, const uint8_t *buf, size_t len, uint8_t retries)`,
    one packet at a time;
  - `int radio_ack_payload(uint8_t pipe, const uint8_t *buf, size_t len)`.
- **`traffic.h`:**
  - `void traffic_start(enum pattern p, uint32_t a, uint32_t b, uint32_t c, uint32_t d)`;
  - `bool traffic_next(uint8_t pipe, struct report *out)`.
  - There is one queue per receiver, 32 deep. A new motion report merges
    into the newest pending motion report that hasn't yet gone to ESB. The
    merge is counted in `merged` and written as a MERGED record. Keyboard reports never merge. On
    overflow the oldest is dropped and counted in `queue_dropped`.
- **Commands:**
  - `kmb role <bridge|receiver|none> <A|B>`;
  - `kmb active <pipe|none>`;
  - `kmb phantom <n>`;
  - `kmb set <name> <value>`;
  - `kmb traffic single <n> <min_ms> <max_ms>`: keyboard reports, which
    never merge;
  - `kmb traffic burst <bursts> <per_burst> <gap_ms> <pause_ms>`: motion
    reports;
  - `kmb traffic stream <hz> <seconds>`: motion reports;
  - `kmb traffic stop`.
- **The B bridge:**
  - It keeps its own queues and gives ESB one packet at a time, in this
    order: the active receiver's reports, then refreshes that are due, then
    pings that are due.
  - With nothing to send, it switches to PRX on pipe 0 with a WELCOME
    preloaded. A report queued while it listens switches it back to PTX.
  - **Phantoms** are linked pipes that never answer. They are pinged like
    receivers and never time out, so they model "the second before that link
    counts as lost" without end.
- **The B receiver:**
  - Unlinked, it is PTX: a HELLO on pipe 0 every 20 ms plus 0–10 ms of
    jitter, then it waits for the WELCOME in the ACK.
  - Linked, it is PRX on its pipe, with a STATUS ACK payload that it
    reloads after every packet.
  - After 1 s without a valid packet the link is lost, and it goes back to
    discovery.

- [ ] **Step 1: Implement** `radio.c`, `traffic.c` and `cand_b.c`, writing
  the records from the table at each event.
- [ ] **Step 2: Build, check the image, and flash both.**
- [ ] **Step 3: Implement `smoke(cand)` in `runs.py`.**
  1. Send `role bridge <cand>` to anduq7 and `role receiver <cand>` to
     macmini, and wait up to 2 s for Link event 3.
  2. Send `active <pipe>`, then `traffic single 200 20 50`.
  3. Collect for 15 s, send `kmb stats` to both and wait for the STATS,
     then run `check_run`, `fit` and `latency`.
- [ ] **Step 4: Run `python -m kmbench smoke B`.** Expected:
  - delivered 200, lost 0, dups 0;
  - a fit with n ≥ 150 and residual p99 ≤ 10 µs;
  - latency p50 printed.
  - If the fit refuses, check the EGU3 priority first (stale stamps).
- [ ] **Step 5: Commit**, message "Add bench radio layer and candidate B".

### Task 6: Candidate A

**Files:**
- Create: `bench/radio-spike/firmware/src/cand_a.c`
- Modify: `cmd.c` (`role … A`)

**Interfaces:**
- Consumes `radio.h`, `traffic.h` and `link.h` from Tasks 4–5.
- **How ESB sends ACK payloads** (v3.4.2, `prepare_ack_pdu_dpl`): each pipe
  has its own queue in ESB. A payload stays at the front until a packet
  with a new PID arrives on that pipe, which shows the receiver got it. ESB
  then drops it, reports TX_SUCCESS, and sends the next queued payload for
  that pipe in the same ACK, or an empty ACK if there is none.
- **The A bridge:**
  - It is PRX on pipes 0–7 at all times.
  - Pipe 0 carries the WELCOME.
  - It hands REPORTs to ESB as soon as they are queued, keeping up to 4 per
    pipe in ESB and topping up on every TX_SUCCESS. Handing them over one at
    a time on TX_SUCCESS would answer every other poll with an empty ACK.
    ESB keeps payloads in order and never overwrites one, as the spec
    requires.
  - It sets `more pending` on a REPORT when more are queued behind it at the
    moment it is sealed.
  - On the active pipe it loads a STATUS, with the active flag, only when
    the flag changes and as the refresh after 25 ms without a REPORT.
    Otherwise a report would wait behind a STATUS sealed before it existed,
    one or two polls longer. Polls in between get empty ACKs, which carry no
    MIC and so don't count as valid packets.
  - On an idle pipe it loads a STATUS whenever ESB holds nothing for that
    pipe.
  - `CONFIG_ESB_TX_FIFO_SIZE` is 32. The default of 8 is shared by all
    pipes, and each linked pipe, phantoms included, holds a STATUS in it.
- **The A receiver:**
  - Unlinked, it sends a HELLO on pipe 0 every 20 ms plus 0–10 ms.
  - Linked and idle, it sends a POLL every 100 ms.
  - Active, it sends a POLL every 500 µs, and immediately after an ACK whose
    `more pending` is set.
  - It follows the active flag in every ACK payload.
- A has no TXDONE on the bridge, so its airtime comes from the receivers'
  TXDONE records.
- Phantoms in A reserve a pipe and a FIFO slot each, and cost nothing on
  air.

- [ ] **Step 1: Implement `cand_a.c`.**
- [ ] **Step 2: Build, check the image, and flash both.**
- [ ] **Step 3: Run `python -m kmbench smoke A`.** Expected: the same as
  Task 5, Step 4.
- [ ] **Step 4: Commit**, message "Add bench candidate A".

### Task 7: Channel scan, test runs and pass/fail

**Files:**
- Create: `bench/radio-spike/firmware/src/scan.c`, `bench/radio-spike/host/kmbench/report.py`, `bench/radio-spike/host/tests/test_report.py`
- Modify: `cmd.c` (`kmb scan <dwell_ms>`), `runs.py`, `__main__.py` (`run`, `report`)

**Interfaces:**
- **Scan:** `kmb scan <dwell_ms>` stops the radio and samples RSSI on each
  of channels 0–80 for `dwell_ms`, then writes a SCAN record per channel.
  `busy_permille` counts samples above −80 dBm.
  - It runs in its own thread. At 200 ms per channel a scan takes 16 s, and
    the main loop must keep feeding the 2 s watchdog and writing the STATS
    that keep the harness from declaring the port stalled.
  - The harness waits for all 81 SCAN records, up to `81 × dwell_ms` plus
    5 s.
- **`runs.py`:** each `run_Tn(cand) -> dict` saves both dongles' raw frames
  under `results/<UTC time>-T<n>-<cand>/{bridge,receiver}.bin`, and the
  metrics in `result.json`. Every run ends with `kmb stats` on both dongles
  after its traffic stops (see `check_run`). T7 reopens the receiver's port
  after each reboot and passes `allow_reboot`.
- **`report.py`:**
  - `judge(results: dict) -> list[Verdict(name, value, limit, ok)]`;
  - `markdown(results) -> str`.

**The tests, run for each candidate unless stated:**

| Test | Commands | Measures |
|---|---|---|
| T1 single | `traffic single 1000 20 200` | latency p50/p99/max, loss, dups |
| T2 load | `traffic burst 50 10 5 300`; `traffic stream 1000 10` | latency, `merged`, `queue_dropped` |
| T3 idle airtime | active receiver linked, no traffic, 60 s | on-air fraction and transaction time; seven-receiver figures computed from both |
| T4 reliability | `traffic stream 200 500` (100,000 reports), owner present; once more with roles swapped, on the chosen candidate | loss per 10,000, retry histogram, dups, `mic_fail` |
| T5 switch | 100 rounds: `active none`, a random 0.3–1.0 s wait, then `active <pipe>` and one report | switch delay p50/p95/max |
| T6 absent receiver | `phantom 1`, then T1 and the stream part of T2 | the same as T1/T2, against the runs without a phantom |
| T7 discovery | 20 rounds of `kmb reboot` on the receiver, idle; for B also under load (bridge active on a phantom, `stream 1000`, report and ping retries 0) | discovery p50/p95/max |
| T7b liveness | `role none` on the receiver | bridge Link event 4 within 1.2 s |
| T8 channels | `scan 200` on both dongles; T4 cut to 20,000 reports on the 3 channels with the lowest combined `busy_permille`, at least 5 channels apart, for the chosen candidate | loss per channel |

A under load in T7 needs a third dongle, so it is reported as not measured.

B under load in T7 uses report retries 0 because every report to the
phantom fails. With 3 retries each one takes at least 4 × 435 µs, longer
than the 1 ms between reports, so the queue never empties and the bridge
never stops transmitting to listen for a HELLO.

**Pass bars, from the spec:**
- latency p99 ≤ 1000 µs in T1, T2 and T4;
- dups = 0 everywhere;
- T4 loss ≤ 1 in 10,000 reports;
- discovery p95 ≤ 1000 ms;
- T6 p99 ≤ 1000 µs;
- T7b ≤ 1200 ms;
- T5 has no bar: it is reported.

**The decision rule, from the spec:** B if B passes; otherwise A if A
passes. If neither passes, report it and stop.

- [ ] **Step 1: Write the failing tests:**
  - `test_judge_passes_good_run`: all metrics at their bars give every
    `ok`.
  - `test_judge_fails_each_bar`: one test per bar, just over it.
  - `test_decision_prefers_B_when_both_pass`.
  - `test_decision_A_when_B_fails`.
  - `test_markdown_has_table_per_test`.
- [ ] **Step 2: Run them to verify they fail. Implement `report.py`; run them to verify they pass.**
- [ ] **Step 3: Implement `scan.c` and the T1–T8 functions.** Build and
  flash both, then do a dry run of T1 with B, cut to 100 reports.
  Expected: a `result.json`, and `python -m kmbench report` prints a table.
- [ ] **Step 4: Commit**, message "Add bench test runs and report".

### Task 8: Bench runs, decision and spec update

**Files:**
- Create: `docs/bench/2026-11-radio-ab.md`
- Modify: `docs/superpowers/specs/2026-11-04-keyboard-mouse-switch-design.md` ("Radio link", "Risks" 3 and 6, "Dongle firmware" identity)

- [ ] **Step 1: Unattended runs.** T1, T2, T3, T5, T6, T7 and T7b for A and
  B (about 40 min). Expected: every run valid under `check_run`.
- [ ] **Step 2: Runs with the owner present.** T4 for A and B, with the
  Keychron keyboard and mouse in use and Wi-Fi busy (2 × 9 min). Note
  whether the owner sees the Keychron mouse stutter under A.
- [ ] **Step 3: Pick the candidate.** Apply the decision rule. Run T4 with
  roles swapped and T8 on the chosen candidate (about 15 min).
- [ ] **Step 4: Write `docs/bench/2026-11-radio-ab.md`.** Cover:
  - the setup;
  - a table per test with both candidates and the pass bars;
  - the decision;
  - the channel and the ESB parameters;
  - the encryption timings;
  - what wasn't measured (A's discovery under load).
- [ ] **Step 5: Update the spec.**
  - "Radio link": the chosen candidate, its channel and ESB parameters, and
    the airtime with one active receiver and with seven known.
  - Risk 3: done, with a link to the results.
  - Risk 6: the placement and P0.19 checks are done; the key-page check
    waits for the real dongle firmware.
  - "Dongle firmware": the identity is confirmed (`E1380B5925A4` matched).
  - The owner reviews the decision.
- [ ] **Step 6: Commit**, message "Record radio bench results and choose candidate".
