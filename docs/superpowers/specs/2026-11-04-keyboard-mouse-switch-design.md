# KeyboardMouseSwitch — design

Date: 2026-11-04
Revised: 2026-11-05, after review
Status: draft, awaiting approval

## Intent

One keyboard and one mouse shared between three computers, with switching that
is instant because no computer ever loses its connection. Every computer sees
an ordinary wired USB keyboard and mouse that is always plugged in; a switch
only changes which computer receives the reports.

The design came out of an earlier chat. It replaced a Bluetooth hub, which was
too slow and dropped links, with USB input devices feeding an RP2350 USB host
plus Nordic nRF52840 dongles talking over a proprietary 2.4 GHz link. The
first input devices are the Keychron 2.4 GHz receivers, but any USB keyboard,
mouse or receiver works. Target latency is about 3–5 ms end to end, against
15–30 ms for Bluetooth.

## Goals

1. **Instant switching.** Switching never makes any computer see a USB or
   radio disconnect.
2. **Caps Lock toggles between the last two computers**, skipping any that
   are offline. No computer ever receives Caps Lock.
3. **Holding Mute and pressing a digit N selects computer N directly.** Mute
   pressed on its own still reaches the computer, as soon as it is released.
4. **Automatic link setup.** No pairing buttons and no per-dongle
   configuration. A dongle plugged into a computer shows up as a new
   computer, with the lowest free number (see "Numbering").
5. **Stable numbering.** A dongle computer gets its number the first time its
   dongle is heard, and the hub stores that number against the dongle's
   factory radio address. The number doesn't change while the computer is off
   or unplugged, or when other dongles are added.
6. **Encrypted radio.** Every radio packet is encrypted and authenticated
   with a pre-shared AES-128 key, with replay protection.
7. **Works before the OS is up.** Each computer sees a USB HID keyboard that
   supports boot protocol, so it works in BIOS/UEFI, at the FileVault prompt
   and at the login screen.
8. **Wakes sleeping computers.** A suspended computer stays selectable, and a
   key press while it is active wakes it.
9. **Low latency.** About 5 ms or less from key or mouse event to the computer
   for dongle computers, and about 3 ms or less for computer 1. See "Latency
   budget".
10. **Reflashing without buttons where possible.** See "Reflashing".
11. **Any USB keyboard, mouse or receiver works as input**, not just the
    Keychron ones: wireless receivers of any brand, combo receivers that carry
    keyboard and mouse together (Logitech, Rapoo and the like), and wired
    keyboards and mice.
    - The hub reads each device's report descriptor; it doesn't rely on
      known device IDs.
    - Several input devices can be plugged in at once. Their keys are merged
      and their mouse movements added together.
    - Caps Lock and the Mute + digit chord work from any keyboard that has
      those keys and reports Mute as held for as long as it is down (risk 7).
    - The Rapoo 5G receiver currently on the Mac mini is a handy second test
      device.

## Non-goals (for now)

- Bluetooth anywhere in the data path.
- Video or audio switching; this is not a full KVM.
- Switching when the pointer hits a screen edge (DeskHop style).
- Remapping modifiers per computer, for example swapping Alt and Cmd for
  Linux versus macOS. The Keychron Mac/Win switch covers this for now.
- Forwarding keyboard LED state (Num/Caps/Scroll) from computers back to the
  keyboard.
- High-resolution scrolling, and mouse report rates above 1 kHz.
- Vendor-specific device features through the switch: Logitech HID++
  pairing, Keychron Launcher/VIA configuration, DPI and lighting setup. To
  configure a device, plug it into a computer directly. The hub ignores
  vendor-defined usage pages.
- Reflashing the hub's own dongle without unplugging it.
- More than 7 dongle computers.
- Windows. *My call:* none of the three computers runs it, so it is not
  tested. Descriptors stay standard so that it most likely works anyway.

## Anti-goals (failure even if it "works")

- Any computer sees a disconnect or re-enumeration when switching.
- A key or mouse button stays pressed on a computer after switching away.
- Keystrokes go over the air in plaintext, or anyone else's device can inject
  into this system.
- The key ends up in git.
- Computer numbers shift because another computer is off or was plugged in
  later.
- Input jumps to a computer the user isn't working on, for example after the
  hub restarts. anduq7 has a logged-in shell on its console, where stray
  typing, a password, or Enter does real damage.

## Hardware (as procured)

| Item | Where | Notes |
|---|---|---|
| Waveshare RP2350-USB-A | anduq7 | RP2350A (QFN60), stepping A4 (marking `RP2350A0A4`), 2 MB flash (W25Q16JV), 12 MHz crystal. USB-C is the RP2350's native USB, with 5.1 kΩ pull-downs on both CC pins. USB-A is PIO-USB on GPIO12 (D+) and GPIO13 (D−), through 27 Ω each. Its VBUS is wired straight to USB-C VBUS, with no switch, fuse or diode. R13 (1.5 kΩ from D+ to 3V3) is fitted for device mode; Waveshare's FAQ says to remove it if devices won't enumerate in host mode. WS2812 RGB LED on GPIO16; BOOT and RESET buttons. Checked against Waveshare's schematic on 2026-11-05. **Still to verify: R13 and charger power-up** (see risk 2). |
| USB 2.0 hub (plain, no card reader) | on the RP2350's USB-A | Runs at full speed behind PIO-USB. |
| Keychron keyboard 2.4 GHz receiver | on the hub | Must work at full speed. |
| Keychron mouse 2.4 GHz receiver (M6) | on the hub | Must work at full speed; a 4K/8K receiver falls back to 1 kHz. |
| nRF52840 Dongle (Nordic PCA10059) | on the hub | Acts as the bridge. Goes on a short USB extension, away from the Keychron receivers. |
| nRF52840 Dongle (Nordic PCA10059) | Mac mini (`Mini-von-Andrei`) | Factory Open DFU bootloader, USB `1915:521f`, serial `E1380B5925A4`. The serial is the dongle's radio address. nRF52840 revision 2; no application installed (checked 2026-11-05). |
| nRF52840 Dongle | MacBook Pro | A third dongle is needed. |
| nRF52840 Dongle (optional) | anduq7 | Test receiver, so the whole radio path can be tested automatically (see "End-to-end testing"). One of the Macs' dongles can be borrowed instead. |

### Computers

- **Computer 1: anduq7 (Linux).** Plugged straight into the RP2350's USB-C,
  which also powers the hub and everything on it (well under 200 mA).
  - anduq7 stays on.
  - **Charger mode.** When anduq7 is shut down for maintenance, its USB-C cable
    is moved to a charger, which is always at hand. The hub must then run
    normally with no host on USB-C: computer 1 counts as offline and the Macs
    keep working. This is supported from the first version. The board has
    5.1 kΩ pull-downs on both CC pins, so a USB-C charger powers it through a
    C-to-C cable; the bench check confirms this once (risk 2).
  - **Later option:** give anduq7 its own dongle and leave the RP2350 on a
    charger permanently.
- **Computers 2 and 3: Mac mini and MacBook Pro.** Each has its own dongle.
  Numbers follow the dongles: the first dongle the hub hears becomes
  computer 2, the next computer 3. Swapping the dongles swaps the numbers; the
  hub console can renumber.
  - **Bring-up order** decides the numbers. Bring up the Mac mini's dongle
    first, then the MacBook Pro's. Only after that, bring up the optional test
    dongle on anduq7, which then becomes computer 4.
  - If the order goes wrong, fix the numbers on the hub console.

## Architecture

```
                                    ┌─ Keychron RX (keyboard)
anduq7 ══USB-C══ RP2350 ══USB-A══ hub ┼─ Keychron RX (mouse)
 (computer 1)    (hub fw)  (PIO-USB)  └─ nRF dongle "bridge" (CDC-ACM)
                                                │  encrypted 2.4 GHz
                                    ┌───────────┴────────────┐
                              nRF dongle "receiver"    nRF dongle "receiver"
                              USB HID → Mac mini       USB HID → MacBook Pro
                              (computer 2 or 3)        (computer 2 or 3)
```

There are two firmware images:

1. **Hub firmware** on the RP2350.
2. **Dongle firmware** on every nRF52840. One image takes either role.

### Hub firmware (RP2350)

- **USB host** on USB-A through Pico-PIO-USB (D+ on GPIO12, D− on GPIO13),
  with TinyUSB host and hub support.
  - HID host for any keyboard, mouse or receiver. It uses a generic
    report-descriptor parser (in `common/`, see "Shared code"), in the way HID
    Remapper does, that handles:
    - report IDs;
    - several HID interfaces on one device;
    - keyboard arrays (6KRO) and NKRO bitmaps;
    - buttons, X/Y, wheel and AC Pan;
    - consumer usages.
  - If a device's descriptor can't be parsed, the hub falls back to boot
    protocol for that keyboard or mouse.
  - When an input device detaches, its keys and buttons leave the merged
    state, and the active computer gets the releases.
  - CDC host for the bridge dongle.
  - **Telling our dongle apart from input devices.** Any USB device can be an
    input, so the hub identifies the bridge in two steps:
    1. **Candidate.** The device has our dongle VID:PID and a CDC-ACM
       interface.
    2. **Confirmed.** The dongle answers an identity query on that CDC port
       with a magic value, protocol version, firmware version and radio
       address.
    
    Only a confirmed dongle becomes the bridge. Its HID interfaces are never
    used as input.
    
    Ignored entirely:
    - a candidate that doesn't answer, or answers with a protocol version the
      hub doesn't support (the hub logs this);
    - other CDC devices;
    - a dongle still in the Nordic bootloader (`1915:521f`).
    
    A protocol version mismatch between hub and bridge is reported. One way
    is for the hub to blink its RGB LED (a WS2812 on GPIO16, driven by a PIO
    state machine that PIO-USB leaves free), so it isn't silent.
  - **Same image, different role.** A dongle on a computer never receives the
    identity query or the "become bridge" command, so it stays a receiver. Only
    the dongle plugged into the hub becomes the bridge. Commands are framed
    (see "Shared code"), so stray bytes on a computer's serial port can't
    trigger them.
  - If two of our dongles are plugged into the hub, the hub uses the one
    with the lower radio address as the bridge and ignores the other.
- **USB device** on native USB-C to anduq7, as a composite device:
  - a boot keyboard: its own interface, boot subclass, no report IDs;
  - a boot mouse: likewise its own interface with no report IDs;
  - consumer control (media keys) on a third interface;
  - the pico-sdk reset interface, so `picotool` can reboot the board into
    BOOTSEL;
  - a CDC port for logs, status and the console. A later pass-through to the
    bridge carries only the bridge's logs and status, and drops report
    frames, for the same reason as the console rule below.
  
  **Console commands.** Production builds offer only status, remove and
  renumber. The command that injects scripted input for the end-to-end tests
  exists only in test builds. In a production build it would let anything on
  anduq7 that can open the hub's serial port (the `dialout` group) type into
  every computer, bypassing the radio encryption. A test build says so in its
  USB product string and its status output, and the end-to-end test script
  flashes the production build back afterwards, so the injection command
  isn't left on the hub by accident.
  
  The USB device declares remote wakeup. Its VID:PID is `2e8a:0001`: the
  Raspberry Pi VID with a PID that Raspberry Pi hasn't allocated. *Owner's
  call (2026-11-05):* picotool skips this PID unless asked for it with
  `--pid 0x0001`, so flashing another RP2350 on anduq7 can't overwrite the
  hub by accident.
  - Its USB serial number is the chip's unique ID
    (`pico_get_unique_board_id_string()`), the same serial the bootrom shows
    in BOOTSEL. After forcing the reboot, picotool drops the PID filter and
    finds the board again by that serial.
  - The hub supplies its own USB descriptors, so it adds the reset interface
    itself. picotool finds it by class, subclass and protocol, not by PID.
- **The system clock must be a multiple of 12 MHz** for Pico-PIO-USB. Use
  120 or 144 MHz, which stay at or below the RP2350's 150 MHz rating, unless
  testing shows otherwise.
- **It owns the switching logic** (see "Switching behaviour"). Computer 1 is
  served directly. Reports for dongle computers go to the bridge over CDC,
  tagged with the destination, following the delivery rule (see "Delivering
  state changes").
- **It keeps the address → number map of known dongles in RP2350 flash**, in
  two reserved sectors at the end of flash outside the firmware image, so
  `picotool load` never overwrites it. This map is what keeps numbering
  stable.
  - Writes are rare: a new dongle, or a console command.
  - Each write goes to the other sector, with a sequence number and a CRC.
    The hub loses power routinely (the cable moves, anduq7 restarts), and a
    power cut during a write then leaves the previous map instead of
    renumbering every dongle.
  - An erase stops execution from flash for tens of ms, and for a few hundred
    ms at the flash chip's worst case.
  - **Preferred:** the PIO-USB core runs from RAM through the write, without
    being locked out, so SOFs never stop.
  - **Otherwise** SOFs stop for more than 3 ms, and every device behind the
    hub sees a suspend, including the bridge.
    - The bridge then releases everything on the active receiver and resumes
      when SOFs return (see "Bridge role").
    - Input devices resume too, though cheap ones may need to re-enumerate.
    - If the builder takes this route, it is documented as a known input
      pause whenever a dongle is added or the console changes the numbering.
- **USB suspend from anduq7 doesn't stop the hub.** The host side keeps
  serving the dongle computers while the USB-C side is suspended.
  - *Revised call:* computer 1 counts as online while suspended (see
    "Online"), so it stays selectable.
  - When computer 1 is active and suspended, the first key press or mouse
    button press wakes anduq7 through remote wakeup.
- **A hardware watchdog** reboots the hub if the firmware hangs. A hung hub
  means no keyboard on any computer. After any restart the hub restores the
  active computer (see "Startup"), so typing doesn't jump to anduq7.
  - **Boot-loop guard.** The hub counts quick watchdog resets in a scratch
    register. After a few in a row it falls back to boot protocol for every
    input device and says so on the console, so one odd device can't take
    the keyboard away from all three computers.

References: HID Remapper and DeskHop, both RP2040/RP2350 projects that use
TinyUSB host with PIO-USB.

### Dongle firmware (nRF52840, nRF Connect SDK / Zephyr)

- **Board:** `nrf52840dongle/nrf52840`. Keep the factory Nordic Open DFU
  bootloader; use the board's default memory layout, which preserves it.
  - Before the first DFU, check that the built image starts at `0x1000` and
    ends below the bootloader.
  - If anything pulls in NCS's Partition Manager (settings or NVS storage,
    for example), it lays out flash itself and can ignore the board's layout.
    A `pm_static.yml` then pins the layout.
- **USB device:** a composite of the same three HID interfaces as the hub's
  USB-C side (boot keyboard, boot mouse, consumer control) and CDC-ACM
  (control link, logs, and a command to reboot into the bootloader).
  - It declares remote wakeup.
  - Its USB serial number is its radio address (see "Identity"), replacing
    Zephyr's default.
- **Receiver role (default).** It presents HID to its computer and plays back
  the reports it receives over the radio.
  - It reports its USB state (configured, suspended, not configured) to the
    bridge, which decides whether its computer is online.
  - When it is active and its computer is suspended, the first key press or
    mouse button press wakes the computer through remote wakeup.
  - **Its radio keeps running while its computer's USB is suspended**, so it
    can still hear the bridge and wake the computer.
    - This draws more current than USB allows in suspend. Macs keep the port
      powered in practice (risk 4).
    - The firmware holds its own request for the high-frequency clock. The
      radio then keeps its clock even when Zephyr's USB driver drops its own
      request on suspend.
  - **It remembers, in RAM, that it was the active computer**, and how long it
    has been without the bridge. When it links up again it reports both, so a
    restarted hub can restore the active computer (see "Startup").
  - It releases everything on its computer by itself when it hears nothing
    valid from the bridge for about 100 ms, or is told it is no longer active
    (see "When a release can't be delivered").
  - **Link loss is a separate, longer timeout.** After about 1 s without valid
    traffic it counts the link as lost: it shows the no-link pattern and
    starts discovery again. One or two lost pings therefore never drop the
    session.
  - Its "time without the bridge" (see "Startup") counts from the last valid
    packet.
  - Its LED:
    - blinks its computer number when it links up;
    - stays lit while it is the active computer;
    - shows a distinct "no link" pattern when it has no link;
    - shows a distinct "version mismatch" pattern when the bridge refuses its
      radio protocol version.
- **Bridge role.** The hub sends a "you are the bridge" command over CDC. From
  then on the dongle:
  - relays reports from CDC to the radio, following the delivery rule (see
    "Delivering state changes");
  - reports the receivers it can hear back to the hub, with their addresses,
    USB state, radio protocol version and whether they are online;
  - sends each receiver its number and whether it is active, for the LEDs;
  - refreshes the active receiver whenever it has sent it nothing for ~25 ms
    (see "Refresh" in "When a release can't be delivered");
  - **When its USB is reset or suspended**, it sends all-released to the
    active receiver and stops sending reports. This happens when the hub
    reboots, loses power, or pauses SOFs during a flash write.
    - After a suspend, it resumes bridging by itself when USB resumes.
    - After a USB reset, it waits for the hub to set it up again.
- **Identity:** the 48-bit radio address from FICR `DEVICEADDR`, derived
  exactly the way the Nordic bootloader derives its USB serial number.
  - The same value then shows everywhere: in the bootloader, in the
    firmware's USB serial, in `ioreg` and `lsusb`, in logs and on the hub
    console.
  - The bootloader most likely forces the top two bits to 1, as for a BLE
    static address. **Confirmed on 2026-11-07 (bench spike):** the
    firmware's USB serial matches the bootloader's on all three dongles
    (`E1380B5925A4`, `DA1EAB2D90C1`, `E2E8D954B732`).
  - Zephyr's default USB serial comes from FICR `DEVICEID`, a different
    register, so the firmware sets the serial itself.
- **A watchdog** reboots the dongle if the firmware hangs.

### Shared code

A `common/` C library is used by both firmware builds and by the host tests:

- the normalized HID report structs and descriptors: keyboard and mouse each
  on their own boot interface with no report IDs, consumer control on a
  third;
- the HID report-descriptor parser and report normalization. They handle
  untrusted input from any USB device, so the host tests run them on
  descriptors captured from the Keychrons and the Rapoo, and fuzz them;
- the CDC frame format, used between hub and bridge and for commands to a
  receiver. Every frame carries a magic value and a CRC, so stray bytes can't
  trigger a command such as reboot-to-bootloader or become-bridge. On Linux,
  for example, ModemManager may probe a new `ttyACM` port;
- the radio packet format, including the nonce layout and the session key
  derivation;
- the switching state machine, which is pure C with no hardware calls, so it
  can be unit-tested on the host;
- the report queue that implements the delivery rule (see "Delivering state
  changes"). Every hop uses it.

### Latency budget

Measured from the moment a report is ready at the input device's USB endpoint
to the moment the computer reads it. The input device's own wireless link
comes on top and isn't ours to control.

| Hop | Dongle computers | Computer 1 |
|---|---|---|
| PIO-USB host polls the input device | ≤ 1 ms | ≤ 1 ms |
| Hub to bridge, CDC OUT in the next frame | ≤ 1 ms | — |
| Bridge to receiver over the air | ≤ 1 ms | — |
| Computer polls the HID endpoint | ≤ 1 ms | ≤ 1 ms |
| Processing and encryption | a few hundred µs | small |
| **Worst case** | **about 4 ms** | **about 2 ms** |

Typical is about half the worst case. The latency goal leaves roughly 1 ms for
the input device's own link, so there is little slack.

The first row assumes a 1 kHz input device, which the Keychron receivers are.
Many cheap keyboards and mice ask to be polled only every 8–10 ms, which adds
up to that much on top of the budget. USB lets the host poll more often than a
device asks, and many devices are fine at 1 ms (Linux's `usbhid.mousepoll`
option relies on this). The bench spike tries this with the devices from
risk 5; if it holds up, the hub polls every input device at 1 ms.

The hub-to-bridge hop is the one to measure first, because it depends on how
Pico-PIO-USB schedules bulk transfers.

## Switching behaviour

Throughout this section, the **active** computer receives input.

### Online

- **Computer 1** is online while anduq7 has configured the hub's USB-C side.
  Suspended still counts as online. In charger mode it is offline.
- **A dongle computer** is online while the bridge has a live, authenticated
  session with its receiver and the receiver's USB is configured by its
  computer.
  - Suspended still counts as online, so a sleeping Mac stays selectable.
  - A dongle that is powered while its computer is off, for example from a
    powered hub, is offline.
- A computer that is shut down but keeps USB power can look exactly like a
  sleeping one, and then stays online. If anduq7 keeps USB power when it
  shuts down, computer 1 stays online until the cable moves to the charger,
  and Caps Lock can still switch to it.
- The **recent list** holds the computers in the order they were last active,
  most recent first. It is kept in RAM and starts empty at boot.

### Startup

**After a restart, the hub restores the active computer.** This keeps typing
from jumping to anduq7's logged-in console shell. The hub looks in two places:

1. **The RP2350's scratch registers.** They survive a watchdog reset and a
   software reset, and most likely a reboot into BOOTSEL for a reflash (the
   builder checks this). They don't survive a power loss.
   - The bootrom and pico-sdk use some watchdog scratch registers for their
     own reboots (at least 4–7), so the hub stays clear of those.
   - POWMAN's scratch registers may be the better choice: they most likely
     survive every reset except power-on.
2. **The receivers.** A receiver that was active, and has been without the
   bridge for less than about 60 s, says so when it links up again (see
   "Receiver role"). This covers a power loss of the hub, for example when
   anduq7 restarts and cuts USB power, or when the cable moves to the charger.
   - Every receiver reports its time without the bridge, whether it claims or
     not. They all lost the bridge when the hub went down, so a claim whose
     time is more than about 5 s longer than the shortest one reported is
     stale and ignored. That receiver dropped out earlier and may have missed
     the switch away.
   - A receiver that hasn't heard the bridge since its own power-up reports
     "never linked" instead of a time, and is left out of the comparison.
     Otherwise one that powered up while the hub was down, for example on a
     MacBook Pro booted meanwhile, would report the shortest time and make a
     genuine claim look stale.
   - If two receivers still both claim, the one with the shorter time without
     the bridge wins.

Until the restored computer is online again, input meant for it is dropped,
just as in "When computers come and go".

**When there is nothing to restore,** active is decided like this:

- At a power-up with no valid scratch registers, the hub first waits for
  receivers to report. It drops input during the wait.
  - The wait runs about 1.5 s from the moment the bridge is confirmed, which
    covers discovery (about 1 s). Counting from power-up would be too short,
    because the USB hub and the bridge must first enumerate behind PIO-USB.
  - If no bridge is confirmed within about 5 s of power-up, the hub stops
    waiting.
  - A claim that arrives after the wait has ended is ignored, so input
    doesn't jump once the user has started typing.
- If no receiver claims to have been active, active is computer 1, even if
  anduq7 hasn't enumerated yet.
- If computer 1 is still offline 3 s after boot (or when the wait ends, if
  that is later), as in charger mode, the lowest-numbered dongle computer
  online at that moment becomes active. If none is online, the first one to
  come online becomes active. Either way, this applies only if the user hasn't
  switched in the meantime.
- After that, a computer coming online never causes a switch.

**Known trade-off: the BIOS hotkey.** On a cold boot of anduq7 the hub powers
up with it and drops input during the wait. That lasts until about 1.5 s
after the bridge is confirmed, at most 5 s. A fast boot can lose the key that
opens BIOS setup; press it again later in the boot, or reboot.

Each of these times is one constant: the 60 s restore window, the 5 s
stale-claim margin, the 1.5 s wait, its 5 s cap and the 3 s grace period.

### Caps Lock

- Caps Lock (keyboard usage `0x39`) is always removed from keyboard reports.
- Pressing it switches to the first online computer in the recent list other
  than active. If there is none, it switches to the lowest-numbered online
  computer other than active. If no other computer is online, nothing happens.

### Mute + digit (chord)

- **Which keys count as Mute:** consumer usage `0xE2`, and keyboard usage
  `0x7F` in case the keyboard sends that instead.
- **Digits:** top-row 1–9 (`0x1E`–`0x26`) and keypad 1–9 (`0x59`–`0x61`).
- **On Mute press,** the hub holds Mute back from the active computer.
- **A digit N pressed while Mute is held** is a command. The digit is dropped
  together with its release:
  - for a known, online computer other than the active one: switch to N;
  - for the active computer, a known but offline computer, or a number with
    no computer: nothing changes.
  
  Further digits while Mute is still held count the same way. Once a digit has
  been pressed, this Mute never reaches any computer; its release is dropped
  too.
- **Mute released with no digit or other key in between:** send Mute (press
  and release) to the active computer. A lone Mute is late only by how long
  the key was held.
- **Any other key while Mute is held:** send the held Mute press to the active
  computer first, then the key. From then on Mute is an ordinary key, and its
  release goes through. Mouse input doesn't count as a key here.
- A digit that was already down when Mute was pressed isn't part of a chord.

### On every switch

1. **The old active computer** moves to the front of the recent list.
2. **The computer being left** gets an all-released state: no keys, no mouse
   buttons, no consumer usage. This prevents stuck keys.
3. **Keys and buttons held at the moment of switching** are masked on the new
   computer until they are released. *My call*, so that nothing half-pressed
   arrives on the new computer.
4. **LEDs:** the dongle LEDs are updated so that only the active computer's
   dongle is lit.

### When computers come and go

- **The active computer goes offline:** active stays where it is, and input
  meant for it is dropped until it comes back or the user switches. *My
  call:* an automatic fallback could send typing to a computer the user isn't
  looking at.
- **A computer comes online:** no switch, except under the startup rule.
- **A suspended computer is selected:** the switch happens as usual, and the
  first key press or mouse button press wakes it.

### When a release can't be delivered

The all-released state on a switch only helps if it arrives. It won't when:

- the radio drops while a key is held;
- the computer being left is out of range;
- the hub reboots or loses power (reflash, crash, cable moved to the
  charger);
- the bridge is unplugged;
- an input device is unplugged mid-press.

A lost release makes the computer autorepeat the key. So:

- **State, not events.** Every report to a receiver carries the full key,
  button and consumer state. Only mouse motion, wheel and AC Pan are deltas.
  Any later report repairs a lost one.
- **Refresh.** When the bridge has sent the active receiver nothing for about
  25 ms, it sends the current full state, with no motion, wheel or pan. A
  refresh goes out only when nothing is queued for that receiver. Otherwise
  it would jump ahead of the queued changes and reorder them.
- **Receiver release timeout.** A receiver that hears nothing valid from the
  bridge for about 100 ms, or is told it is no longer active, sends
  all-released to its computer by itself.
  - Key repeat counts from the press, so a key whose release is lost stays
    down for its real hold time plus up to 100 ms. A quick tap then stays
    under the shortest common repeat delays (about 225 ms on macOS, 250 ms
    on the Linux console). A key already held close to the repeat delay may
    still repeat once.
  - After a release of this kind, keys that the next report still shows as
    held stay masked until they are released, as on a switch, so no key is
    pressed twice.
  - Link loss is a separate, longer timeout (see "Receiver role").
- **Bridge losing the hub.** When the bridge's USB is reset or suspended, it
  sends all-released to the active receiver and stops. The receiver timeout
  covers the case where the bridge loses power instead.
- **Device detach.** When an input device detaches, its keys and buttons leave
  the merged state, and the active computer gets the releases.
- **No backlog.** A stalled bridge gets the current state when it recovers,
  not a backlog of old reports. "Delivering state changes" says what may be
  merged and dropped.

Each of these constants is defined in one place, and the builder may tune them
within these bounds.

### Delivering state changes

Sending only the latest state can lose a key press entirely. A press and its
release that fall inside one sending interval cancel out, and nothing is sent
at all. The Mute tap the chord produces, a press and release at the same
moment, would never reach a Mac.

This rule applies on every hop: the hub's USB-C endpoints, hub to bridge over
CDC, the radio (including ACK payloads), and the receiver's HID endpoints.

- **What may be merged.** Only mouse motion, wheel and pan, which are added
  up, and repeated identical states. They merge only into the newest pending
  report, never across a key or button change, so clicks and drags start
  where the pointer was. If a sum would overflow its report field, a new
  report starts instead; motion is never clipped.
- **Nothing else is merged away.** Every change to keys, buttons or consumer
  usages goes out in order, as its own report.
  - **Order holds within each USB interface:** keyboard, mouse and consumer
    control. The computer reads the three endpoints independently, so order
    across them isn't guaranteed and doesn't need to be.
  - Each interface has its own queue. One shared queue would halve the mouse
    rate while typing.
- **USB endpoints.** A report is written to an IN endpoint only after the
  computer has read the previous one, which TinyUSB and Zephyr both signal.
  A made-up tap, such as the held-back Mute, is then simply two reports in the
  queue, and the computer sees both.
- **Best effort on the radio.** *Owner's call (2026-11-05):* an occasional
  lost key press is acceptable, because you press again when nothing
  happens. Stuck keys and doubled presses are not acceptable.
  - **Retransmits.** ESB retransmits each packet a few times until it is
    acknowledged, and nothing is doubled. In B the receiver's ESB layer
    discards a retransmission it has already received. In A the bridge
    answers a repeated poll with the same ACK payload and moves on only when
    a new poll arrives. In both, the replay counter rejects a duplicate
    anyway.
  - **Loss.** A report still lost after all retries stays lost. A quick tap
    can then vanish, but a lost release is repaired by the next report or the
    refresh, which carry the full state.
  - **No application-level resends.** The bridge doesn't resend reports
    itself, so mouse motion is never doubled.
  - **No sequence numbers.** There are no sequence numbers or delivery
    acknowledgements beyond ESB's own.
- **Overflow.** If changes pile up beyond a small queue, for example while the
  bridge is stalled, the oldest changes may be dropped and replaced by the
  current state. A press is never dropped without its release.
  - *My call:* a short burst of lost typing is better than a backlog arriving
    late.

### Numbering

- Computer 1 is always anduq7 (the hub's USB-C port).
- Dongle computers are numbered 2 to 8. When the hub completes its first
  authenticated session with a dongle it doesn't know, it gives the dongle the
  lowest free number and stores address → number in flash.
- A number never changes by itself: not while its computer is off or
  unplugged, and not when other dongles are added.
- If all seven numbers are taken, a new dongle isn't added; the hub logs it.
- *My call:* removing a dongle (which frees its number) and renumbering (for
  example, swapping 2 and 3) happen through commands on the hub's console.
  There is no hotkey for either.
- *My call:* remove refuses a dongle that is linked, and the console says to
  unplug it first. Linked means it has a live session with the bridge, even
  when its computer is off and the computer therefore counts as offline. A
  linked dongle would otherwise be added straight back with the lowest free
  number. Renumber works on linked dongles.

## Radio link

Requirements:

- Low latency for the active receiver: about 1 ms or less over the air,
  including retransmits in normal conditions.
- Automatic discovery. A receiver that powers up is known to the hub within
  about a second.
- Liveness. The hub knows within about a second when a receiver disappears.
- **Encryption:** AES-128-CCM or an equivalent AEAD, with a pre-shared key and
  a MIC on every packet. All dongles share one key, so:
  - **Session keys.** At discovery each side contributes a fresh random salt,
    and both derive a session key from the pre-shared key and both salts, for
    example SK = AES-128 under the pre-shared key of
    (salt_bridge ‖ salt_receiver). All traffic in the session uses SK. This is
    the scheme BLE's link layer uses.
  - **Nonces.** A direction bit plus a per-direction packet counter that
    starts at 0 each session. Because SK is unique per session, no nonce
    repeats across receivers or reboots.
  - **Replay protection.** Each side rejects a counter that isn't higher than
    the last one it accepted. This survives a reboot of either side without
    manual action, because a reboot means a new session.
  - **Authenticated discovery.** Discovery messages carry a MIC under the
    pre-shared key. A session counts as established only when the first packet
    under SK verifies. Only then does the hub add a new dongle to flash, and
    only then does a new session replace an older one with the same receiver.
    Otherwise anything on the discovery channel could fill the seven numbers,
    wear out the flash, or knock a receiver off by replaying an old hello.
- **Versioning.** Discovery carries the radio protocol version. A receiver
  with a version the bridge doesn't support isn't admitted. The bridge reports
  it to the hub, which logs it, and the receiver shows its version-mismatch LED
  pattern. Receivers are flashed one at a time, so this will happen.
- **Airtime.** A packet plus its ACK carrying about 25 bytes (reports, counter,
  MIC) takes roughly 300–450 µs at 2 Mbit/s, including ramp-up and
  turnaround. The chosen design states how much of the channel it uses with a
  dongle computer active but no input, and with seven receivers known. Less is
  better: the bridge sits next to the Keychron receivers.
- Coexistence with Wi-Fi and with the Keychron receivers' own 2.4 GHz links.
  - Pick a fixed channel, or a hopping set, away from the common Wi-Fi
    channels.
  - The bridge goes on a short USB extension, away from the Keychron
    receivers.

Two candidates go into the bench spike (risk 3), and the builder picks one from
the results.

**A. Polling (Gazell-style),** on Nordic ESB, or Gazell itself, which ships
with nRF Connect SDK. The bridge is the PRX, which listens continuously;
receivers are PTX and poll it.

- The active receiver polls as fast as one transaction allows, realistically
  about every 500 µs, and the bridge delivers its reports in ACK payloads.
  - The payloads are queued in order under the delivery rule.
  - A payload that is waiting is never overwritten with a newer state.
- Idle receivers poll about every 100 ms, which doubles as their keepalive.
- A receiver learns that it is active, or no longer active, from an ACK
  payload. A payload carries a "more pending" flag, and the receiver keeps
  polling fast until it clears, so the receiver being left gets its last
  reports, its all-released and its "not active" without waiting an idle
  interval for each.
- Each known receiver gets its own pipe, so ACK payloads reach the right
  receiver. Pipe 0 is for discovery.
- Discovery is easy: a new receiver simply starts polling on pipe 0.
- Cost: the channel is 60–90% busy whenever a dongle computer is active,
  input or not, and idle receivers' polls and discovery collide with that
  traffic. A receiver becoming active hears so only at its next idle poll,
  so the first input after a switch can wait up to ~100 ms, unless idle
  receivers poll faster, which costs airtime.

**B. Bridge transmits on demand,** on Nordic ESB.

- Receivers are PRX and listen on their own address. The bridge is PTX and
  sends a report to the active receiver as soon as the report arrives: one
  transaction, no airtime between inputs, and lower latency than waiting for a
  poll.
- The bridge pings each linked receiver about every 100 ms for liveness and
  LED state, and refreshes the active one whenever it has sent it nothing for
  ~25 ms. A receiver that isn't linked gets no pings; discovery brings it
  back. Receivers return their status, such as USB state, in ACK payloads.
  ESB loads an ACK payload before the next packet arrives, so this status is
  always one transaction old, which is fine for status.
- **No receiver holds up another.** ESB sends from one queue, strictly in
  order, and a packet that fails all its retries stays at the front until
  the application removes it. So the bridge keeps its own queue per receiver
  and gives ESB one packet at a time, reports for the active receiver first.
  Pings use few retries, and the bridge removes a failed packet itself.
  Otherwise each ping to a receiver that has just gone out of range would
  hold up the active receiver's reports by a few ms, for the second before
  that link counts as lost.
- Discovery: an unlinked receiver periodically switches to PTX and sends a
  hello to a discovery address, which the bridge listens on as PRX whenever it
  isn't transmitting.
- Cost: both sides switch radio modes, and a report that arrives while the
  bridge is listening waits for the switch.

*My recommendation:* B, unless the spike shows that mode switching or
discovery costs more than expected. B also has no idle-poll delay when
switching.

**Chosen: B** (owner, 2026-11-07, after the bench spike; results in
`docs/bench/2026-11-radio-ab.md`). By the bench's bars B misses four:
- latency in T1 and T2, by under a microsecond (p99 1,300.6 and 1,300.1 µs
  against 1,300);
- the active receiver's latency while another linked receiver doesn't
  answer (T6);
- discovery while input streams (T7 under load).

Two of the rules below address the last two: PINGs to idle receivers go
out once, and the bridge keeps a listening window between reports, with
little work around each switch. A missed three latency bars, and its
discovery under load was slower than a second.

- **Measured** on channel 45, 2 m apart, with encryption, from the bridge's
  queue to the receiver's application:
  - median 720 µs, p99 1.28–1.30 ms;
  - 0.5 reports in 10,000 lost to the radio over 100,000 reports, plus one
    dropped by ESB after it was acknowledged (see the packet-ID rule);
  - discovery 2.6 ms (p95), link loss noticed in 1 s.

  A's median is about 916 µs, and A takes 195 ms to switch computers against
  B's 0.7 ms.
- **Airtime:** 0.97% of the channel with one receiver active and no input;
  about 2.2% with seven receivers known. A's figures are 30% and 31%.
- **ESB parameters:**
  - 2 Mbit/s, dynamic payload length, 16-bit CRC, fast ramp-up, 0 dBm;
  - retransmit delay 435 µs (ESB's minimum);
  - report retries 3, HELLO retries 0;
  - PING every 100 ms, refresh after 25 ms, link lost after 1 s.
- **Channel:** none is fixed yet. The bench used 45 (2445 MHz); 11, 16 and
  39 also tested clean that evening. Which channels are usable changed
  within the day (Wi-Fi on channels 1 and 4 nearby, Keychron receivers
  across the band). So the hub should be able to move the link to another
  channel, rather than rely on one fixed channel.
- **Rules the bench added:**
  - **ESB's radio interrupt runs at the highest priority.** The PRX has the
    TX ramp-up time (40 µs with fast ramp-up) to set up each ACK. A late
    interrupt sends the received packet back as the ACK. At ESB's default
    priority, behind USB and the system timer, this happened 16–25 times per
    run.
  - **PINGs to idle receivers go out once, without retries.** A second
    attempt to a receiver that has just gone away holds up the active
    receiver's reports by up to 0.9 ms.
  - **The bridge keeps a short listening window between reports,** about
    0.3 ms. Without it, a receiver can't join while input streams at 1 kHz:
    0 of 20 joined on the bench. Keep the work around each switch small:
    seal the WELCOME once, seal the next report before switching, and
    don't re-initialise ESB if it can be avoided. On the bench that
    overhead was about 300 µs per gap.
  - **ESB's packet ID continues across re-initialisation.** `esb_init`
    resets it, and the receiver's ESB drops a packet whose ID and checksum
    both match the previous one, after acknowledging it. With the ID
    restarting at every switch, about one report in 65,536 vanishes with no
    retry possible. Keep the sequence going (for example with
    `esb_reuse_pid()` after each `esb_init`), or switch modes without
    re-initialising ESB.
  - **Retries on one channel don't beat a periodic interferer.** If the link
    hops, it hops on retry or varies the retry delay.
  - **Placement:**
    - dongles stay away from USB 3 hubs and cables (one beside a dongle
      raised its noise floor by 12 dB);
    - away from other 2.4 GHz receivers;
    - off the floor next to the PC.

    Distance (2 m, one dongle under a table) was never the problem: both
    ends received at about -62 dBm.

## Key handling

- `~/.kmswitch-key` holds the AES-128 key as 32 hex characters. It already
  exists on the Mac mini, readable only by its owner.
- The dongle build reads it and generates a header in the build directory. The
  key never appears in the source tree.
- **Every dongle `.hex` and DFU `.zip` contains the key.** `.gitignore` covers
  the build directories, not just generated key files. A DFU zip copied to
  another computer for flashing is treated as secret and deleted afterwards.
- **If the file is missing, the build fails** with a message saying how to
  create one and that every dongle must then be reflashed. The build never
  creates a key silently.
- The hub firmware does not need the key, because its link to the bridge is
  wired USB.
- **Threat model.** One key unlocks every dongle. Assume that anyone with a
  few minutes of physical access to any dongle can read it out: every `.hex`
  and DFU `.zip` contains the key, the debug port may be open, and the reset
  button puts a running dongle into its bootloader.
  - **If a dongle or a DFU zip goes missing, or a computer with a dongle may
    be compromised:** make a new key and reflash every dongle.
  - **The build machine.** The Mac mini holds the key file, so software
    running as its owner can read the key directly. It is trusted anyway,
    because it builds every image.
- **Checked on 2026-11-05,** on the Mac mini's dongle through read-only
  bootloader queries, and in Nordic's source for this open bootloader (nRF5
  SDK 15 to 17 agree; the factory build itself isn't published):
  - the chip is an nRF52840 revision 2 (FICR variant `AAD0`). Each new dongle
    is checked the same way before its first flash, with
    `tools/dongle-info.py` while it is in its bootloader, because the
    revision decides how the debug port is locked;
  - the bootloader takes application and SoftDevice images unsigned, and
    bootloader images only with Nordic's signature;
  - it enters DFU only when there is no valid application, or after a pin
    reset. It ignores GPREGRET (see "Reflashing");
  - an upload erases only the flash pages it covers, starting at `0x1000`.
    The rest of the old image stays readable by the new one;
  - the 12 KB below the bootloader (`NRF_DFU_APP_DATA_AREA_SIZE`) is never
    erased by an update.
- **Software on a computer can't reach the key.** On macOS any local process
  can open a dongle's serial port (`crw-rw-rw-`), send the framed
  reboot-to-bootloader command and upload its own image. The dongle firmware
  makes sure that image finds no key:
  - every copy of the key lives in the image's first flash page
    (`0x1000`–`0x1FFF`), which any upload erases before its code runs. The
    key is defined in one source file and seen elsewhere only as an
    `extern`, so the compiler can't copy its bytes into other code;
  - a build check fails unless the key's bytes appear in the image exactly
    once, inside that page;
  - the key is copied into RAM only while a session key is derived, and
    wiped right after. Before rebooting into the bootloader, the firmware
    wipes all key material from RAM, session keys included;
  - nothing derived from the key is written to flash.

  Replacing a dongle's firmware stays possible, but gains that software
  nothing it doesn't already have on its own computer. So the reboot command
  needs no authentication.
- **Debug port (APPROTECT), optional.** On revision 2 the debug port is open
  unless UICR.APPROTECT is written. nrfx's startup lock
  (`CONFIG_NRF_APPROTECT_LOCK`) acts only on newer revisions, so the dongle
  firmware writes UICR.APPROTECT itself on first start. Newer revisions lock
  the port after every reset unless firmware opens it, which Zephyr does
  only when UICR allows it.
  - Updates through the bootloader keep working.
  - Unlocking a dongle over SWD erases it completely, bootloader included.
    Restoring the bootloader then needs the J-Link and a cable to the
    dongle's SWD pads.
  - On revision 2 it stops a casual flash dump over SWD, not a determined
    attacker: the lock on these revisions is publicly known to be bypassable
    by voltage glitching. That falls under physical access above.
- To build on anduq7 as well, copy the file there.

## Reflashing

- **RP2350:** from anduq7, run `picotool load -x -f --pid 0x0001 hub.uf2`.
  The reset interface reboots the board into BOOTSEL, so no button is needed;
  the BOOTSEL button remains the fallback.
  - The board's USB-A VBUS comes straight from USB-C VBUS (see "Hardware"),
    so the hub behind it stays powered through the reboot. While the RP2350
    is quiet, the hub suspends its ports, so the bridge first sees a USB
    suspend, and a USB reset only when the new firmware enumerates the hub
    again. The bridge releases everything on the active receiver at the
    suspend (see "Dongle firmware").
- **Receiver dongles:** a framed CDC command makes the dongle wipe its key
  material from RAM and pull P0.19 low. On the PCA10059 that pin is wired to
  reset (Nordic's own USB DFU trigger does the same), and the bootloader
  enters DFU after a pin reset. Then `nrfutil` flashes a DFU zip. The side
  reset button remains the fallback.
  - Nordic's published configuration of this bootloader ignores GPREGRET
    (`0xB1`). Check the P0.19 reset on the first build.
- **Bridge dongle:** move it to a computer's USB port and flash it like a
  receiver.
- **The Mac mini loses its keyboard and mouse while flashing.** Its input
  comes through the switch: its receiver disappears during its own update,
  the bridge is off the hub while it is flashed, and after a radio protocol
  change it has no input until both ends match. So:
  - flash scripts need no input once started: they wait for the dongle,
    reboot it into the bootloader, flash it and check the result;
  - a radio protocol change flashes the bridge and the Mac mini's receiver in
    one run;
  - anduq7 keeps its keyboard throughout, so a shell from anduq7 to the Mac
    mini is the fallback. SSH works in both directions.
- **Build machine:** the Mac mini builds both images. The hub UF2 is copied to
  anduq7 for flashing, for example with `scp` plus `ssh anduq7 picotool …`
  wrapped in a script.
  - Key-based SSH works in both directions between the Mac mini and anduq7.
  - `picotool` 2.3.1 is installed on anduq7 by the anduhome playbook (tag
    `picotool`). It reaches the RP2350 (an RP2350A, QFN60) as andrei without
    sudo.
  - Its udev rules cover the standard Pico PIDs (`2e8a:0003`, `0009`, `000a`,
    `000f`), not the whole VID. The hub's `2e8a:0001` has its own rule,
    `/etc/udev/rules.d/61-picotool-extra.rules`, which anduhome manages
    (`dev_x64_computer.yml`, variable `picotool_extra_usb_ids`). If the hub's
    PID ever changes, that variable changes with it.
  - On 2026-11-04 the RP2350 was enumerating on anduq7 as `2e8a:000f`
    (RP2350 Boot, BOOTSEL mode).

## End-to-end testing on anduq7

anduq7 is Linux, so tests read what the hub sends straight from the kernel's
input layer instead of watching a shell:

1. A test script on the Mac mini starts a small python-evdev script on
   anduq7 over SSH. It grabs (`EVIOCGRAB`) the hub's input devices under
   `/dev/input/by-id/`, and for the radio path the test dongle's. The grab
   keeps the events away from anduq7's console while the test runs.
2. The same process opens the hub's CDC console in a test build and sends
   the scripted sequence one step at a time, each step only after the
   previous one has been read back. The injection command exists only in
   test builds (see "Hub firmware").
3. It compares the events it read with the expected ones: every usage with
   press and release, including that Caps Lock never arrives, Mute and other
   consumer keys, mouse buttons and motion, and that nothing stays pressed.

The hub forwards usages and never translates characters, so checking usages is
the right level. The keymap of anduq7's console is anduq7's business.

**The radio path.** With a receiver dongle plugged into anduq7 as well (see
"Hardware"), the same check covers hub → bridge → radio → receiver → anduq7's
input layer, with no macOS permissions needed. The test dongle gets a computer
number like any other.

**Needs:**

- **Reading input events.** The test user must be able to read the hub's and
  the test dongle's event devices. andrei was added to the `input` group on
  anduq7 by hand on 2026-11-05. This is temporary: the owner drops it after
  testing. The hub is anduq7's keyboard, so read access to its event devices
  means reading everything typed on anduq7.
- **The hub's serial port.** The test script opens the hub's CDC console.
  andrei is already in `dialout` on anduq7, through anduhome's ESP-IDF task.
- **Test tools.** anduhome installs python3-evdev, and `evtest` for manual
  checks.
- **ModemManager.** anduhome adds a udev rule so ModemManager ignores the
  hub's `2e8a:0001` and the dongles' VID:PID (`ID_MM_DEVICE_IGNORE`). It
  probes new serial ports on Linux.
- **The test console.** Test builds of the hub include the injection command.
  The test script flashes the production build back afterwards (see "Hub
  firmware").

**Safety rule:** if the grab is lost mid-run, for example because the SSH
session dies, the rest of the test input lands in anduq7's logged-in console
shell. So:

- The test build stops injecting as soon as its console port is closed. If
  the SSH session dies, the process holding the grab and the port dies with
  it, so at most the step in flight reaches the shell.
- Enter appears in a test sequence only where a case needs it.
- The first run is confirmed with the owner beforehand.
- Autorepeat and stuck-key handling are tested in host unit tests before
  they are ever pointed at anduq7.

On the Mac side, enumeration is checked with `ioreg`. Keystrokes are checked
manually, or with a small HID listener; on macOS that needs the Input
Monitoring permission.

## Toolchain

- **Hub:** C, pico-sdk 2.x, TinyUSB (bundled with pico-sdk), Pico-PIO-USB,
  CMake + Ninja, `picotool` (on anduq7 for flashing).
- **Dongles:** C, nRF Connect SDK (installed with `nrfutil toolchain-manager`
  or nRF Connect for VS Code), `west`, and `nrfutil` with its DFU command for
  flashing.
- **Host tests:** CMake on macOS, running the `common/` logic.
- **End-to-end tests on anduq7:** python-evdev, and `evtest` for manual
  checks, both installed by anduhome.
- **Installed on the Mac mini today:** CMake, Ninja, J-Link, Rust and Python.
- **Missing on the Mac mini:** `arm-none-eabi-gcc`, pico-sdk, `picotool`,
  nRF Connect SDK, `west` and `nrfutil`. Installing them is part of the work.

## Proposed repository layout

```
README.md            overview, hardware, how to build and flash
.gitignore
common/              shared C: HID parser, reports, framing, packets, switching
hub/                 RP2350 firmware (pico-sdk CMake project)
dongle/              nRF52840 firmware (NCS application)
tests/               host unit tests for common/
tools/               flash and key helper scripts
docs/                design, hardware notes, bench checklist
```

## First milestone: "the switch works"

On the bench, with anduq7 and the Mac mini:

- typing and mouse work on anduq7 through the RP2350;
- Caps Lock moves keyboard and mouse to the Mac mini and back instantly;
- Mute + 2 selects the Mac mini (computer 2, provided its dongle is the first
  one the hub hears; see "Computers");
- after a hub restart, typing goes back to whichever computer was active;
- no key gets stuck, including when the radio link drops or the hub is
  power-cycled with a key held down on the Mac mini;
- the radio link is encrypted.

The MacBook Pro joins once its dongle is flashed.

### Risks to check first, before building on them

1. Both Keychron receivers and the hub enumerate behind PIO-USB on this
   RP2350 board, and their reports arrive at full speed.
2. The board itself. Waveshare's schematic settles the pins, the VBUS path
   and the CC pull-downs (see "Hardware"). The chip is stepping A4 (package
   marking `RP2350A0A4`, read by the owner on 2026-11-05), so erratum
   RP2350-E9, which upsets PIO-USB's pull-downs on A2, doesn't apply.
   Still open:
   - whether the hub enumerates and stays stable with R13 fitted. Only the
     hub, a full-speed device, sits directly on the port, so R13 most likely
     does no harm. It does keep D+ high, so the port most likely never sees
     the hub unplugged; replugging the hub may then need a hub restart.
     Removing R13 is the fallback;
   - that the board really powers up from a USB-C charger through a C-to-C
     cable. Charger mode depends on it.
3. Latency, airtime and reliability of radio candidates A and B, with
   encryption on. Include the active receiver's latency while another linked
   receiver is unplugged.
   **Done, 2026-11-07:** candidate B. See "Radio link" and
   `docs/bench/2026-11-radio-ab.md`.
4. The dongle's HID plus CDC composite device works on both macOS and Linux,
   and macOS accepts it as a keyboard (the Keyboard Setup Assistant may appear
   once). Two further checks:
   - while each Mac sleeps, its dongle keeps hearing the bridge;
   - a key press wakes the Mac mini and the MacBook Pro.
5. Other input devices behind the hub:
   - low-speed (1.5 Mbit/s) devices, which many cheap wired mice and
     keyboards are, behind the full-speed hub on PIO-USB. The specific
     question is whether Pico-PIO-USB sends the PRE packets that low-speed
     devices behind a hub need;
   - a combo receiver, such as the Rapoo 5G, with several interfaces and
     report IDs.
6. The NCS build places the dongle image at `0x1000`, below the bootloader
   (see "Dongle firmware"). Check this before the first DFU, together with
   two things from "Key handling": the key lands only in the image's first
   flash page, and the reboot command (P0.19) reaches the bootloader.
   **Partly done, 2026-11-07 (bench spike):**
   - The placement check works: `tools/check-dongle-image.py` checks every
     image.
   - The P0.19 reboot reaches the bootloader on both bench dongles.

   The key-page check waits for the real dongle firmware, since the bench
   uses a public key.
7. Mute is reported as held. The chord needs the keyboard to report Mute as
   down for as long as the key is held. Check the Keychron in both Mac and Win
   modes, and the Rapoo. Some keyboards send media keys as an instant press
   and release, and on those the chord can't work. While at it, note which
   usages the Keychron's Mac-mode keys (Mission Control, Launchpad and the
   like) send: a vendor-defined page would be dropped silently (see
   "Non-goals").

## Left to the builder

- The CDC frame format between hub and bridge, within the rules in "Shared
  code".
- The exact radio protocol: which candidate, channel(s), pipe and address
  assignment, and the timing constants within the stated requirements.
- The crypto implementation: the nRF52840's CryptoCell through PSA, the ECB
  peripheral with software CCM, or the CCM peripheral, which implements a
  BLE-style counter-and-direction nonce in hardware.
- How work is split across the RP2350's two cores, and the task structure.
- The details of report normalization, including NKRO-to-6KRO conversion and
  how mouse fields are mapped.
- USB VID:PID for the dongles. The pid.codes test PID `1209:0001` is fine for
  private use. The hub's `2e8a:0001` is fixed (see "Hub firmware").
- The exact LED patterns.
- Logging and the console command set, including remove and renumber. The
  production console offers only status, remove and renumber.
- The timing constants, within the bounds above:
  - startup grace period;
  - restore window, stale-claim margin, and the wait for receivers at
    power-up with its cap;
  - state refresh interval;
  - receiver release timeout and link-loss timeout;
  - report queue depth.
