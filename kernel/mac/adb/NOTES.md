# ADB on the Quadra 800

## Files

- `adb.c`: bus layer for the VIA1 shift-register transceiver.
  - Per-byte state machine: `adb_intr`, IPL 4.
  - Request queue and packet ring.
  - SRQ scan and auto-poll target.
  - Watchdog: `adb_tick`.
  - Polled enumeration with collision resolution: `adb_init`, from `io_init`.
- `adbkbd.c`: keyboard. Decodes talk R0. Has a raw consumer hook (A/UX shape) and the console path: US layout to ASCII, VT100 arrows, auto-repeat, and the Caps Lock LED through R2.
- `adbms.c`: mouse. Decodes R0 into `adb_mouse_x/y/button` and has a consumer hook. No handler change and no extended protocol.
- `test/`: host simulator of VIA1, the transceiver and devices, with checks of the bus layer, enumeration, SRQ handling and keyboard decoding (`sh test/run.sh`).
- `verify.sh`: static checks on the linked image.

## VIA1 use

| Resource | Use |
|---|---|
| PB3 (input) | /INT from the transceiver. Sampled at each SR interrupt; meaning below |
| PB4, PB5 (outputs) | ST0, ST1: 00 CMD, 01 EVEN, 10 ODD, 11 IDLE |
| SR | command and data bytes |
| ACR bits 4–2 | 111 = shift out, 011 = shift in, both on the external clock (CB1 from the transceiver) |
| IFR/IER bit 2 | SR interrupt; `adb_intr` acknowledges it with IFR = 0x04 |
| DDRB | bits 4, 5 outputs, bit 3 input. PB0–2 (RTC) and PB6/7 are left alone |

**Protocol**

- Every command starts from IDLE. Order: SR to output, write the command, then ST = CMD.
- Talk: at the command's SR interrupt, switch SR to input and set EVEN. Then toggle EVEN/ODD once per byte.
- Listen: write each data byte to SR, then toggle.
- A transaction ends with the next command, or with IDLE. In IDLE the transceiver repeats the last talk by itself (auto-poll).

**PB3 low at an SR interrupt means:**

| When | Meaning |
|---|---|
| After the command byte | another device asserts SRQ |
| On data byte 0 | no reply. For an auto-poll this means SRQ |
| On data byte 1 | end of reply; the byte is kept |
| On data bytes 2–7 | end of reply; the byte is filler |

At most 8 bytes are read.

**Interrupt path (`macintr.s` p1int)**

- The SR bit is served before T1.
- `adb_intr` runs at IPL 4, as A/UX's `Xfdbint` does, so a level-4 SCC interrupt cannot delay a state change.
- `adb_soft` then runs at IPL 1. It calls completions and decoders, and so the keyboard input path.
- A tick pending at the same time re-enters p1int, because VIA /IRQ is a level signal.
- The tick path calls `adb_tick` at IPL 2, before `clock_int`, for the watchdog and key repeat.

## Timing (ADB specification)

| Item | Value |
|---|---|
| Bit cell | 100 µs |
| Attention | 800 µs |
| Sync | 65 µs |
| Stop-to-start (Tlt) | 140–260 µs |
| SRQ | 300 µs low in the stop bit of a command addressed to another device |
| Reset | 3 ms low |
| A talk with a 2-byte reply | about 3 ms |

**Delays and bounds**

- Delays count VIA reads. Each read takes at least 1 µs (E-clock synchronised), so waits are never shorter than stated.
- After reset: 5 ms. After an address move: 1 ms.
- Each polled operation has a budget of 30 000 VIA reads (≥ 30 ms).
- Watchdog:
  - A transaction that makes no progress for 6 ticks (100 ms) is abandoned and the bus restarted.
  - A request that has waited 2 ticks in IDLE behind a low PB3 is started anyway.

**Boot bounds**

- If the reset and two of the first talks get no SR interrupt at all, the driver prints `adb: transceiver not responding, ADB disabled` and returns. Worst case is about 0.1–0.2 s.
- With a working bus, enumeration is 15 talk R3, then 5 operations per device per pass (2 passes).
- All waits are bounded; ADB failures never panic.

## Enumeration and collisions

1. Reset, then talk R3 at addresses 1–15. The table is indexed by current address and records the default address (the device class) and the handler.
2. For each occupied address *a*, move a device to the highest free address *f*:
   - talk R3 *a*
   - listen R3 *a* with `{0x60|f, 0xFE}`. Only a device that saw no collision on the last talk R3 moves.
   - talk R3 *f*
3. If *a* still answers, the moved device stays at *f* and the loop continues.
4. Otherwise the moved device was alone, and it is moved back.
5. Two passes, at most 4 moves per address per pass.

## Auto-poll and SRQ

- The auto-poll target is the keyboard (address 2). With no keyboard it is the first device found, else 2, so a keyboard plugged in later is still polled.
- **SRQ:** the driver talks R0 to each device in turn (at most 16 steps) until the SRQ goes away. The device that answered becomes the auto-poll target.
- Talk-R0 data is routed by the device's default address: 2 is the keyboard, 3 the mouse. Unknown addresses 2 and 3 are routed the same way.

## Keyboard → console

- Bytes go to `adb_ttyin`, one at a time.
- When some Mac source defines `fbcons_input()`, `build.sh` compiles with `-DADB_FBCONS` and `adb_ttyin = fbcons_input`, the video console's input hook: bytes go up the console tty (major 0). `fbcons.h` allows any IPL.
  - The check is whether a line starting with `fbcons_input(` appears in `mac/*.c` or `mac/*/*.c`. `ADB_FBCONS=1` or `ADB_FBCONS=0` overrides it.
- With no definition, `adb_ttyin = 0` and keystrokes are dropped (there is no screen console then either).
- `verify.sh` reports which case was linked.
- The keyboard feeds the existing console tty rather than its own STREAMS tty: console selection is by `coinfo` (cdevsw[0] and `oncons`), a second tty driver would duplicate termios handling and need its own major, and the translation layer stays reusable for other console drivers.

**Map**

- US ANSI, by raw ADB key code:
  - Shift, Control, Caps Lock (latching: down while locked).
  - Return and keypad Enter give CR. Delete gives BS (0x08). Esc, Tab.
  - Arrows give `ESC [ A/B/C/D`. Forward Delete gives DEL.
  - Keypad digits and operators.
  - Right-hand modifier codes 0x7B–0x7D (handler 3).
- Command combinations, Option and function keys produce nothing.
- Control: `@`–`~` becomes `& 0x1F`; space and 2 give NUL, 6 gives RS, `-` and `/` give US.
- **Auto-repeat:** first repeat after 30 ticks, then every 3 ticks (A/UX's `KEY_DEFWAIT` / `KEY_DEFGAP`).

**Console tty modes.** The AMIX default `VERASE` is `#` (`CERASE`) and `VINTR` is DEL. The Mac Delete key sends BS, as the Mac KCHR does. The root image's `/etc/ioctl.syscon`, which `init` applies to the console, sets erase ^H, kill ^U, intr ^C and `ECHOE` (`ramdisk/NOTES.md`).

## Raw event interface (for uinter)

The shape matches A/UX 3.1's key/mouse layer, which `UI_DEVICES` hooks (derived from the shipped `/unix`):

- `UI_devices` calls `key_open(0, UI_keyboard, KEY_ARAW)` and `mouse_open(0, ui_mouse, 1)`, or `key_op`/`mouse_op(…, INTR, fn)` when the devices are already open.
- **Keyboard.** `key_intr` in ARAW mode calls `fn(unit, KC_CHAR=2, code, more)` for each R0 byte that is not 0xFF:
  - first the high byte, with `more` = (low byte ≠ 0xFF);
  - then the low byte, with `more` = 0;
  - R0 = 0x7F7F (power key) gives 0x7F twice, `more` 1 then 0.

  `UI_keyboard` ignores 0x7F, maps raw codes to virtual codes (table `kmapData`: raw 0x36→0x3B, 0x3B–0x3E→0x7B–0x7E, 0x7B–0x7D→0x3C–0x3E) and updates `KeyMap`.
- **Mouse.** `mouse_intr` calls `fn(unit, MOUSE_CHANGE=1, (short)R0, changed)`, where `changed` has bit 0 for the button and bit 1 for motion. `mouse_button[]` and `mouse_x/y[]` hold the state.

  A/UX `mouse_intr` also adds the deltas to Mac low memory: `MTemp` $828 v / $82A h, only if `CrsrBusy` $8CD = 0, then sets `CrsrNew` $8CE = 0xFF. That belongs in the uinter consumer.
- **Here:**
  - `adb_keyhook(fn)` and `adb_mousehook(fn)` return the previous hook. They use the same call shapes.
  - A key consumer takes the keyboard away from the console, as with A/UX's ownership. Hook 0 gives it back.
  - State is kept in `adb_keydown[16]` (key-down bitmap, for `UI_GETKEYS`), `adb_key_r0`, `adb_mouse_x/y/button`.
- **Open:**
  - `KEY_OP_*` / `MOUSE_OP_*` beyond the hook.
  - Register-2 raw delivery (`KC_RAW2`).
  - Extended mouse (handler 4) and the 200 cpi handler 2.
  - Keyboard handler 3.

## Sources

- NetBSD mac68k `adb_direct.c`, `akbd.c`, `ams.c` (BSD) for the `ADB_HW_II` bit usage, the PB3 end rules and the enumeration outline.
- A/UX 3.1 `/unix`: `fdb_inthand`/`Xfdbint`/`via_fdb_start`/`fdb_init`, `key_intr`, `mouse_intr`, `UI_devices`, `UI_keyboard`, `ui_mouse`, `kmapData`.
- The ADB specification and Inside Macintosh for key codes and register layouts. The keymap follows the public key-code chart and matches the unshifted and shifted tables of the A/UX kernel's US `KCHR` (`transData`).

## Open hardware questions

1. **PB3 end-of-reply rule.**
   - Does a 2-byte reply signal the end on byte 1 (NetBSD's reading) or on the filler byte 2? Both are handled.
   - Does PB3 read low right after a command when another device has SRQ pending? A/UX `S0End` and NetBSD `POLLING` both treat it so.
2. **Every command starting from IDLE.** A/UX does this. Whether the brief IDLE between back-to-back commands starts a colliding auto-poll would show as `adb_nwdog` counts.
3. **No delay before sampling PB3.** A/UX samples at once; NetBSD waits 150 µs. If replies come back truncated, add a short `adb_delay` in `adb_intr`.
4. **LED bits in R2 byte 1** (bit 0 Num, 1 Caps, 2 Scroll, active low), and whether an Apple Extended Keyboard drives Caps itself.
5. **io_init IPL and timing.** Enumeration is polled with the SR interrupt off. Interrupts are enabled at the end.
6. **Counters** to read on hardware:
   - `adb_nintr`, `adb_nspur` (SR interrupts in IDLE with PB3 high)
   - `adb_ntmo` (no-reply talks)
   - `adb_nsrq`
   - `adb_nwdog` (restarts)
   - `adb_nlost` (packet ring overflow)
