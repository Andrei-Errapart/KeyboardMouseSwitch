# Radio bench: candidates A and B

Spec risk 3: latency, airtime and reliability of the two radio candidates,
with encryption on. Run on 2026-11-07 with the bench spike in
`bench/radio-spike/` (plan: `docs/superpowers/plans/2026-11-07-radio-bench-spike.md`).
The bench README has the toolchain, the record stream and the interference
notes from the day.

## Decision

**Candidate B: the bridge transmits on demand.** Owner's call, 2026-11-07.

B is faster in the typical case: its median latency is 715 µs, against
about 920 µs for A, which waits for the next poll. B switches computers in
0.7 ms against A's 195 ms. It keeps 2% of the channel busy with seven
receivers known, against A's 31%. And under interference it loses fewer
reports.

By the plan's rule (B if B passes every bar, else A if A passes, else
neither), the result is "neither". B misses two bars:
- **T6.** Its PINGs to an absent receiver take two attempts. With one
  attempt it passed, in a single run that wasn't saved (see T6).
- **T7 under load.** No receiver joined while the bridge streamed, because
  B's bridge listens for HELLOs only when it has nothing to send. Fixing
  that takes a listening window between reports (Finding 3): a design
  change, not a setting.

A misses three, T1, T2 and T6, and wasn't run in T4 or T7 under load. In
the third-dongle version of T7 under load, A's discovery p95 was 2,333 ms,
which fails too.

The latency bar is 1.3 ms, not the plan's 1 ms. Latency is measured from
the bridge's queue to the receiver's application, so it includes
encryption at both ends. The spec's latency budget lists that separately
("processing and encryption: a few hundred µs") from "over the air ≤ 1 ms".
Owner's call, 2026-11-07.

The 1.3 ms is a judgement, not a figure the measurements give: the dongles
themselves account for about 600 µs of B's 715 µs median (see "How latency
is measured"), twice the 300 µs the bar allows for them. The bar was set
after the first runs, at 21:08 UTC, when B's p99 of 1,296 µs had already
shown up twice (20:41 and 20:46 UTC). That is 4 µs under the new bar. At
the plan's 1 ms, B would also fail T1, T2 and T4; A's misses stay the
same. The result is "neither" under either bar.

## Setup

| | |
|---|---|
| Bridge | Dongle `DA1EAB2D90C1` on anduq7, on a 4-port USB 2.0 hub off the floor, beside the RP2350 at the end of its extension cable. The Keychron receivers sit on the same hub, a little apart: closer than Finding 5 advises. |
| Receiver | Dongle `E1380B5925A4` on the Mac mini's hub, about 2 m from the bridge. |
| Third dongle | `E2E8D954B732`, beside the receiver on the Mac mini: a second receiver or a controlled interferer (extra experiments only). |
| Input devices | The Keychron keyboard and mouse worked over Bluetooth to the Mac mini during the runs. Their 2.4 GHz receivers carried no input, but still transmitted (likely looking for their devices). |
| Signal | Both ends received the bench packets at about -62 dBm (-54 to -66 dBm over the day). |
| Channel | 45 (2445 MHz) for every run except T8. It was clear at both ends; the neighbouring 2.4 GHz Wi-Fi networks use channels 1 and 4. |
| Firmware | `023019e` for T1–T3 and T5–T7b, `10021eb` for T4 (both ways round) and T8 (adds `kmb set defaults` only), and `b228535` for the extra experiments (adds the interferer). The T4 and T8 records show `08332ff-dirty`: the same code, flashed just before it was committed as `10021eb`. |

### Radio parameters

Nordic ESB from NCS v3.4.2, with:
- 2 Mbit/s, dynamic payload length, 16-bit CRC, fast ramp-up, 0 dBm;
- retransmit delay 435 µs, ESB's minimum;
- ESB's radio interrupt at priority 0, its event interrupt at 2 (see Findings).

| Setting | B | A |
|---|---|---|
| Report retries | 3 | — (polls: 3) |
| PING retries | 1 | — |
| HELLO retries | 0 | 0 |
| Refresh of the active receiver | 25 ms | 25 ms |
| PING interval | 100 ms | — |
| Poll interval | — | 500 µs active, 100 ms idle, 5 ms while linking |
| Link counted as lost after | 1 s | 1 s |
| HELLO interval while unlinked | 20 ms + 0–10 ms random | same |

On-air sizes: a report is 25 bytes, PING, POLL and STATUS are 16, HELLO is 20
and WELCOME is 14.

### Encryption

AES-128-CCM with a 4-byte MIC, sealed with the ECB peripheral and software
CCM. It took 94 µs to seal and 95 µs to open a 17-byte body, and 12 µs to
derive a session key. PSA Crypto took 240–254 µs to seal or open on both
the CC310 and Oberon drivers (172–251 µs for a session key), so the bench
doesn't use it.

The self-test checks NIST SP 800-38C examples 1 and 2, the bench's packet
layout, replay rejection and a tampered packet.

### How latency is measured

Latency runs from the moment a report enters the bridge's queue to the
moment the receiver's application has decrypted it.

Both dongles timestamp every radio ADDRESS event with a 1 MHz timer. Packets
that got through on the first try fit one clock to the other, with a 99th
percentile residual of 0.9–2.8 µs (6.9 µs in T4 with the roles swapped).

B's typical 715 µs breaks down as:
- 374 µs inside the bridge, of which about 95 µs is sealing;
- 116 µs on air;
- 225 µs at the receiver, of which about 95 µs is opening.

About 200 µs inside the bridge isn't accounted for yet.

## Results

Pass bars:
- latency p99 ≤ 1,300 µs in T1, T2, T4 and T6 (the plan had 1,000 µs; see
  Decision);
- duplicates = 0;
- T4 loss ≤ 1 in 10,000;
- discovery p95 ≤ 1,000 ms in T7, idle and under load;
- link loss noticed within 1,200 ms.

| Test | What it does | B | A |
|---|---|---|---|
| T1 single reports | 1,000 reports, 20–200 ms apart | p50 715, **p99 1,296**, max 1,875 µs; 0 lost | p50 920, **p99 1,520**, max 2,275 µs; 0 lost |
| T2 load | bursts, then a 1 kHz stream for 10 s (10,500 reports) | p50 830, **p99 1,296** µs; 0 lost | p50 900, **p99 1,448** µs; 0 lost |
| T3 idle airtime | active receiver linked, no input, 60 s | 0.97% busy; with seven receivers 2.2%; 337 µs per transaction | 29.7% busy; with seven receivers 31%; 237 µs per transaction |
| T4 reliability | 100,000 reports at 200 Hz | **0.6 lost per 10,000**, p99 1,279 µs; second attempt 4.8%, fourth 0.05%; one MIC failure | not run (owner's call: A was out) |
| T4, roles swapped | the Mac mini dongle as the bridge | **0.2 lost per 10,000**, p99 701 µs | — |
| T5 switch | delay from `active` to the first report | p50 0.69, p95 0.69, max 1.3 ms | p50 195, p95 286, max 297 ms |
| T6 absent receiver | T1 and the stream with one receiver that never answers | **p99 1,542 µs** (fails); 0 lost, 24 merged | **p99 1,523 µs** (fails); 0 lost |
| T7 discovery | receiver reboots 20 times | p50 2.5, **p95 2.6 ms** | p50 3.1, **p95 3.1 ms** |
| T7 under load | the same, while the bridge streams 1,000 reports a second to a receiver that never answers | **0 of 20 joined** (fails) | not run: needs a third dongle (see Extra experiments) |
| T7b liveness | receiver stops | **986 ms** | **1,002 ms** |
| T8 channels | scan, then 20,000 reports on each of the three quietest channels | ch 11: 0.5, ch 16: 0, ch 39: 0.5 lost per 10,000; p99 1,278 µs on each | — |

No run had a duplicate. In T3, the seven-receiver figures add six idle
receivers' PING or POLL traffic to the one measured.

Notes on the results:
- **B's p99 of 1,296 µs is a report retried once.** One retry costs
  1,278–1,296 µs across runs, against the 1,300 µs bar, so B passes as long
  as fewer than 1% of reports need two or more retries. In T4, 5.2% needed
  a retry and 0.43% needed two or more. The margin is thin: the
  once-retried latency varies by 18 µs between runs, and the bar is only
  4 µs above its highest value.
- **T4's pass isn't clear-cut.** With 6 losses in 100,000 reports, the
  one-sided 95% upper bound is about 1.2 per 10,000, above the bar.
- **In T4 with the roles swapped, most retries were lost ACKs.** The
  receiver already had the report. The Mac mini end receives worse than
  anduq7's: the Keychron keyboard and mouse were talking Bluetooth to it.
- **B's T6 miss is the second attempt of each PING to the absent
  receiver.** That second attempt holds a report up by up to 0.9 ms. While
  reports waited, 24 were merged into the motion report queued ahead of
  them; none were lost. With PINGs sent once: p99 1,268 µs and 11,000 of
  11,000 delivered (one run, not saved). A real absent receiver is dropped
  after 1 s, so this case lasts about a second each time a receiver
  disappears.
- **T2 had one echo** (see Findings), out of about 11,000 transactions. In
  B the report still arrived; only that ACK's STATUS was lost. T4 logged
  one MIC failure too, in about 100,000 reports; its records don't show
  whether it was an echo.

### Runs

The figures above come from these runs in `bench/radio-spike/results/`
(times in UTC; local time was an hour later):

| Test | B | A |
|---|---|---|
| T1 | `20261107T211020Z-T1-B` | `20261107T211218Z-T1-A` |
| T2 | `20261107T211252Z-T2-B` | `20261107T211326Z-T2-A` |
| T3 | `20261107T211431Z-T3-B` | `20261107T211537Z-T3-A` |
| T4 | `20261107T220456Z-T4-B` | — |
| T4, roles swapped | `20261107T221324Z-T4swap-B` | — |
| T5 | `20261107T211728Z-T5-B` | `20261107T211919Z-T5-A` |
| T6 | `20261107T212126Z-T6-B` | `20261107T212335Z-T6-A` |
| T7 | `20261107T212448Z-T7-B` | `20261107T212600Z-T7-A` |
| T7 under load | `20261107T212905Z-T7-B` | — |
| T7b | `20261107T212606Z-T7b-B` | `20261107T212612Z-T7b-A` |
| T8 | `20261107T221900Z-T8-B` | — |

The extra experiments are the `J…` and `T7third` runs from 22:29 on.

The earlier runs in the same directory don't count:
- **18:02–18:05, channel 77:** a short pass of each test (T1 with 100
  reports, T5 with 4 rounds, T7 with 2).
- **19:32–19:57, channel 45:** T1 runs on the way to the setup above. B lost
  13–15% of reports and A 5–81%; "Controlled interference" matches these.
  A's first three also had ESB's radio interrupt at its default priority,
  with 16–25 MIC failures each.
- **20:40–20:47:** short T1 runs (200 reports) after the priority change,
  before the full runs.

## Extra experiments (third dongle)

These go beyond the plan, at the owner's request. They use the third dongle
and firmware `b228535`.

### Discovery while the bridge streams to a real receiver

The bridge streamed 1,000 reports a second to the active receiver, while
the third dongle rebooted 20 times.

| | Joined | Discovery | The active receiver meanwhile |
|---|---|---|---|
| B | **0 of 20** within 5 s (4,564 HELLOs, 1 acknowledged) | — | 165,474 delivered, 0 lost, p99 1,422 µs |
| A | 20 of 20 | p50 570 ms, **p95 2,333 ms**, max 3,523 ms | 894 of 75,502 lost, p99 6.1 ms |

B's bridge listens for HELLOs only when it has nothing to send. Under a
steady 1 kHz stream it goes back to listening only briefly between reports,
too briefly to catch a HELLO. A new receiver therefore waits for the first
pause in input.

The plan's version of this test (T7 under load) had the bridge stream to an
absent receiver that is never dropped. There B got 0 of 20 too.

### Controlled interference

`kmb jam <ch> <percent>` sends 25-byte dummy packets with random gaps.
`kmb jam off` reports the share of time actually on air, which is lower
than asked because of timer granularity.

The interferer sat beside the receiver. At 0 dBm it reached the receiver
at about -21 dBm, as strong as the Keychron bursts beside the receiver
earlier that day. Each point is 300 single reports on channel 45.

On our channel, at 0 dBm:

| Interferer on air | B first try | B lost | A first try (polls) | A lost |
|---|---|---|---|---|
| 4.3% | 91% | 0 | 94% | 0 |
| 7.6% | 84% | 0 | 88% | 0 |
| 13.2% | 68% | 4 | 79% | 9 |
| 19.7% | 57% | 20 | 70% | 74 |
| 24.8% | 46% | 143 | 63% | 193 |

Power and separation, at 19.7% on air:

| Interferer | B first try | B lost | A first try | A lost |
|---|---|---|---|---|
| Our channel, -40, -20, -12, -4 or 0 dBm | 56–63% | 12–25 | 69–75% | 48–78 |
| 2 MHz off, -40 dBm | 99% | 0 | — | — |
| 2 MHz off, -20 to 0 dBm | 57–66% | 17–24 | 70% (0 dBm) | 74 (0 dBm) |
| 4 MHz off, 0 dBm | 86% | 0 | — | — |
| 6 MHz off, 0 dBm | 93% | 0 | 97% | 0 |

What this shows:
- **On our channel, power doesn't matter.** Even at -40 dBm the interferer
  reaches about our own signal level (-62 dBm), and any overlap destroys
  the packet.
- **Two MHz off, it matters.** An interferer at about our level is
  harmless; 20 dB stronger, it is as bad as one on our channel.
- **A strong nearby transmitter needs 4–6 MHz of separation.**
- **The evening's failures (the 19:32–19:57 runs, see "Runs") match an
  interferer on our channel about 20–22% of the time.** That's about half of first attempts failing and
  7–13% of reports lost.
- **A loses more reports than B at the same interference,** even though its
  short polls get through more often. That's ESB's ACK-payload behaviour:
  see the bench README, "Candidate A loses reports where B doesn't".
- **Above about 20% on air, the interferer is almost periodic** (about a
  565 µs cycle). Retries at ESB's fixed 435 µs spacing then keep colliding:
  at 24.8%, 81% of B's failed first attempts never got through. A random
  interferer lets most retries through; a periodic one, such as a 1 kHz
  receiver poll, doesn't.

## Findings that shape the dongle firmware

1. **ESB's radio interrupt must have the highest priority.** ESB's PRX
   turns from receive to transmit in hardware for the ACK. Its interrupt
   then has the TX ramp-up time (40 µs with fast ramp-up) to point the
   radio at the ACK.
   - If it's late, the radio sends the packet it just received back as the
     ACK.
   - At ESB's default priority 1 (shared with USB and the system timer), A's
     receiver got 16–25 of its own POLLs back per run, counted as MIC
     failures. A's bridge then also dropped the payload that ACK should have
     carried.
   - At priority 0 it fell to two MIC failures, so at most two echoes (one
     in T2, one in T4), in about 1.06 million transactions in the runs in
     the table (865,000 of them first-try; T8's records weren't saved):
     roughly one in 500,000. So something still holds interrupts off for
     more than 40 µs now and then. That's worth finding in the product
     firmware, or turning fast ramp-up off (129 µs).
2. **PINGs to idle receivers go out once, without retries.** Otherwise a
   receiver that has just gone away holds the active receiver's reports up
   by up to 0.9 ms until it is dropped (T6).
3. **The bridge needs a guaranteed listening window between reports**, about
   0.3 ms. Otherwise a receiver can't join while input streams.
4. **Retries on a single channel don't beat a periodic interferer.** If the
   product hops, hop on retry, or vary the retry delay.
5. **Placement matters more than distance.** Most of the day's losses came
   from where things sat, not from the 2 m between the dongles:
   - Under the table next to the PC, the noise floor was -78 to -80 dBm. Off
     the floor by the RP2350, it was -87 to -91 dBm.
   - Two Keychron receivers beside the receiver dongle left it receiving
     poorly: 13 of 14 failed PINGs were packets it had to receive.
   - A USB 3 hub beside a dongle raised its noise floor from about -82 to -70 dBm,
     and it received nothing.

   So the rules are: keep dongles away from USB 3 hubs and cables, away
   from other 2.4 GHz receivers (at least a short extension), and off the
   floor next to the PC.
6. **The usable channels change within minutes.** Channel 80 went from 40 of
   40 PINGs acknowledged to 1 of 15 later the same day, and the Keychron
   receivers moved across the band. A fixed channel is fragile; the hub
   should be able to change channel.

## Not measured

- **A's T4.** A was out by then (owner's call).
- **A's T7 under load, in the plan's form.** The third-dongle version under
  "Extra experiments" stands in for it.
- **B's T6 with PINGs sent once, on record.** One run passed, but it wasn't
  saved.
- **Coexistence with Keychron 2.4 GHz links that carry input**, next to the
  bridge. During the runs the keyboard and mouse used Bluetooth. Re-check
  this once the hub forwards input.
- **T4 with busy 2.4 GHz Wi-Fi**, which the plan asked for. The Mac mini's
  own network is on 5 GHz; the 2.4 GHz networks nearby are the neighbours'.
- **B with a listening window between reports.** That's a firmware change
  for the product; it hasn't been tried.
- **The nRF52840's CCM peripheral**, which would likely be faster than
  ECB with software CCM.
- **The hub-to-bridge USB hop**, which needs the hub firmware (latency
  budget, "Hub to bridge").
