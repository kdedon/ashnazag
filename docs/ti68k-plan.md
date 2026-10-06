# TI-89 / TI-92 Plus / Voyage 200 environment: feasibility plan

Status: research from public hardware notes (TIGCC/GCC4TI headers, TiEmu/TilEm register docs). Nothing run.
Addresses below are from memory of those notes; stage 0 confirms each against the ROM before relying on it.

## 1. Hardware AMS touches
- CPU: 68000 (89 HW1, 92+, V200) or 68000-class with HW2 ASIC tweaks. Supervisor mode throughout; AMS owns the vectors.
- RAM: 256 KB at $000000 (mirrored), shared with the LCD buffer (the buffer address is a port, so it is plain RAM).
- Flash: 89 2 MB, 92+ 2 MB, V200 4 MB, from $200000. Boot code in the first sectors; AMS follows; the rest is the user archive.
- I/O $600000 range: memory-protection and wait-state bits, RAM/ROM size, hardware version (HW1/HW2 probes), LCD address and contrast, link-port lines, battery/ADC, keyboard row mask and column read, RTC/timer rate, power-down.
- I/O $700000 range: HW2 additions (extra timer, link/keyboard variants, Flash protection hooks).
- Interrupts: auto-vector levels AI1-AI6: programmable timer, ON key, link activity, timer pair and the slow tick (exact numbering from the docs). Power-down is HALT/STOP plus an ON-key wake.
- LCD: 160x100 (89) or 240x128 (92+/V200), 1 bit, row-major, DMA from RAM by the ASIC. Contrast is a port write.
- Keyboard: matrix of rows (written) and columns (read); ON key separate and wakes from STOP.
- Link: 2-wire bit-banged (read/write line bits plus an interrupt on change); AMS runs the protocol in software.
- Flash writes: AMD-style command sequence (unlock cycles, program, erase sector, status polling by toggle bit) executed from RAM; protected by a port bit that boot code sets and AMS toggles.

## 2. Register-stub layer
Easy: LCD address/contrast (latch, point the display service at that RAM), keyboard matrix (answer rows from the host key state), timers (host clock raises levels 1-5), link (idle lines), battery (full), hardware-version (report HW2 or HW1 consistently with the ROM), RAM-size and wait-state bits (accept).
- Flash: map the Flash image read-only; trap writes. Decode the command state machine (6 states, status polling) in the host and apply to the image file. Erase-sector, program-word and sector protect are all that AMS uses. Cheap, but AMS runs the sequence from RAM with timed polls, so the stub must complete instantly.
- Protected memory: AMS toggles bits that make some RAM ranges (the OS-reserved and archive-copy areas) fault on write; the fault is an address-error-style trap AMS handles. Stub with the guest framework's page protection, or ignore (AMS does not rely on a fault in normal use). Decide at stage 3.
- Boot code: the hard part. AMS calls entry points in the boot code (Flash program/erase, OS checks, certificate and unit-ID reads) and verifies the boot version on start. Either supply the user's boot dump, or a stub that exports the same jump table. The table list is the census output.
- Unknown registers: log-and-return-zero mode (census) before stubbing each.
- Not required: ASIC internals, LCD timing, link electrical model, HW1 quirks beyond the version probe.

## 3. ROM sourcing
- AMS: TI's free OS upgrade files (.89u/.9xu). Header plus signed image; we strip the header and place the image at the Flash OS address ourselves. No signature check needed since we do not run the boot upgrade code.
- Boot code: not in the upgrade file. Options: (a) the user's own dump (boot1/boot2, taken from their calculator) copied to the environment folder; (b) our own minimal stub: reset vector, supervisor setup, RAM clear, ports to the HW2 defaults, jump table for Flash and unit-ID calls, then jump to the AMS entry.
- Feasibility: AMS from an upgrade file plus our stub is likely, not certain: it needs the stub's jump table to match every boot call AMS makes, and AMS reads certificate/ID data (stub with fixed values). Also needs a plausible Flash state: the upgrade's trailing "OS valid" markers and an empty archive area (erased bytes). Stage 1 answers this by census; if too many calls, fall back to (a) and document it as user-supplied.
- Ship nothing: ROM image, upgrade files and dumps stay local in ~/TI89/<env>/ (per env: flash.img, ram if kept, boot.bin).

## 4. Display, input, stages
- Display: 1-bit buffer at the LCD address, fed to the display service as a monochrome window (160x100 or 240x128, integer scale, white-on-grey LCD palette). Update on a frame timer plus on address/contrast writes; no per-line timing.
- Input: map host keys to the matrix: letters to the alpha keys, Shift = 2nd, Ctrl = diamond, Alt = alpha, F1-F5 to the softkeys, Esc = ON, Enter, Backspace = clear/backspace, cursor keys to the pad; mouse optional. On-screen keypad via the display service later. V200 qwerty maps 1:1.
- Link: host file or socket via the existing guest services; idle first. Archive and Flash persist through flash.img.
- Stages and tests (QEMU q800 and Falcon Hatari only through the usual runners, later):
  0. Census: run AMS (upgrade plus a throwaway boot stub) under the guest framework with all I/O in logging mode; output: the register list and boot call list. Test: log has no fault and reaches the first key read.
  1. Boot stub plus register stubs; AMS reaches the HOME screen. Test: LCD buffer hash after 3 s matches the stored home-screen reference.
  2. Display service window and keyboard: type 2+3 Enter, read 5. Test: scripted keys, screen hash.
  3. Flash model and archive: archive a variable, reset, find it again. Test: flash.img differs only in the expected sector.
  4. Timers, power-down (STOP and ON wake), contrast, auto power-off. Test: idle 60 s then ON key wakes.
  5. Link to a host file; app and OS install from a file; second model (92+/V200 240x128). Test: send a program, run it.
- Unknowns: boot call surface size, timing-sensitive Flash polls, HW1 versus HW2 behaviour in one AMS, protected-memory dependence, whether the archive garbage collector needs real erase timing.
