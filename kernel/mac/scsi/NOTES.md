# Quadra 800 53C96 SCSI for the AMIX kernel

53C96 host adapter, Apple partition map and root on disk. The partition parser and the chip
state machine have host tests; hardware behaviour listed under "Open hardware questions" is
unverified.

## Files

| file | role |
|---|---|
| `ncr96.c`, `ncr96reg.h` | 53C96 host adapter: per-target queues, interrupt-driven state machine with disconnect/reselect, pseudo-DMA and PIO, watchdog |
| `ncr96pdma.s` | `ncr_blind`: the 256-byte blind chunk, read or write, with a `u_nofault` pad |
| `scsimac.c` | the `sd.h` layer: `sdopen`/`sdqueue`/`sdhardwarename`, `sdpartition`/`sdvalid`/`sdblkno`/`sddevsize` |
| `apm.c`, `apm.h` | Apple Partition Map → slices; no kernel headers, also built on the host |
| `macspl.h` | `splscsi`/`splrestore` (see "Compiler trap") |
| `build.sh` | builds `macscsi.o` (`ld -r` of the above) into an output directory |
| `trial-link.sh` | applies the SCSI integration to scratch copies and links the kernel |
| `test/run.py`, `test/apmtest.c` | host test of the partition scan |
| `test/ncrtest.sh`, `test/ncrsim.c`, `test/ncrhost.h` | host test of `ncr96.c` against a simulated 53C96 and disks |

## Layering

AMIX's SCSI stack has three layers:

1. **Device drivers** building CDBs: `dd` (disk, block + raw, bmaj 18 / cmaj 40), `ct` (QIC
   tape), `gsioctl` (pass-through).
2. **The `sd.h` layer**: `struct sdcom` requests, `sdqueue()` routing, completion through
   `cp->intr`, slices through `sdpartition` and friends.
3. **Host adapter drivers** (`a3091queue`, `a2091queue`, …).

`dd` supplies block and raw entry points, the `physio` bounce (`amiga_dma_pageio`),
READ(10)/WRITE(10) with REQUEST SENSE and one retry on CHECK CONDITION (absorbing the unit
attention after our bus reset), `ddsize`, the `DIOC`/`DIOCHARDWARENAME` ioctls and the
`cNdYsZ` minor layout. The Mac port replaces layers 2 and 3 and keeps `dd`, `ct` and
`gsioctl` from the base, so the root-device mechanism (`rootdev` = bmaj 18) and `/dev` names
are AMIX's.

- **Address.** `dd` passes `vtop(b_addr)`. Kernel RAM is identity-mapped (`kpseg`, VA = PA
  below `MAINSTORE+VSIZOFMEM`; `vtop040` returns identity for kernel VAs below 0x40000000), so
  the CPU copy loops use it directly.
- **Contiguity.** Buffer-cache and `amiga_dma_pageio` bounce buffers are physically
  contiguous.
- **Caches.** Pseudo-DMA is a CPU copy and the 68040 data cache is physically tagged, so no
  flush is needed; the A3091-specific `dma_cache040` hooks do not apply. The chip, DMA port
  and VIA2 (0x50F00000–0x50FFFFFF) are mapped cache-inhibited serialized by `mac_iomap`
  (`PG_IO` 0xC1), so port writes stay ordered against later FIFO-count reads.

## Minor numbers and slices

Per `sd.h`: bits 0–2 = target, bit 3 = card (0 on the Q800), bits 4–6 = slice. Slice 0 is the
whole disk: `sbBlkCount` from the Driver Descriptor Map, or AMIX's "unknown" 1234567 without a
DDM. Slices 1–7 come from the Apple Partition Map, read at each open.

### Partition-type convention

`apm_scan` reads block 0 and entries 1…`pmMapBlkCnt` (at most 64). A DDM `sbBlkSize` other
than 0 or 512 counts as no map.

**Skipped** (case-insensitive): `Apple_partition_map`, `Apple_Driver*`, `Apple_FWDriver*`,
`Apple_Free`, `Apple_Void`, `Apple_Patches`, empty type, zero length.

**Assigned in passes; the first claim of a slice wins:**

1. `Apple_UNIX_SVR2` whose A/UX block-zero block (bzb, in `pmBootArgs`, magic 0xABADBABE)
   holds an explicit `bzb_slice` (flags bits 0–4, "slice + 1") of 1–7 → that slice.
2. `Apple_UNIX_SVR2` with a bzb, by role: root flag (0x8000) → s1; `bzb_type` 3 (swap) → s2;
   usr flag (0x4000) → s3. These are A/UX's default slices 0/1/2 plus one (`sys/gdisk.h`).
3. `Apple_UNIX_SVR2` without a bzb (as `pdisk` writes), by name substring: "root" → s1,
   "swap" → s2, "usr" → s3.
4. Remaining `Apple_UNIX_SVR2`, in map order → lowest free of s4–s7.
5. Remaining data partitions (Apple_HFS, Apple_Scratch, …) → lowest free of s4–s7.

So `/dev/dsk/c0d0s1` is root and `c0d0s2` swap on the internal disk, matching AMIX's own
convention. Without a bzb, name the partitions "Root" and "Swap". To share a disk with A/UX,
give the AMIX partitions explicit `bzb_slice` values so they claim s1/s2 first; A/UX's land in
s4–s7. A separate disk is simpler.

The A/UX 3.1 image (`AUX_3_1_1GB`, host test `auxdisk`):

| slice | entry | blocks | reason |
|---|---|---|---|
| s0 | — | 0 + 2048000 | DDM |
| s1 | 5 "UNIX Root&Usr slice 0" | 96 + 1769468 | bzb root (flags 0xC000) |
| s2 | 6 "Swap" | 1769564 + 131070 | bzb type 3 |
| s4 | 3 "Eschatology 1" | 2041856 + 6144 | other SVR2 |
| s5 | 4 "MacOS" | 1900634 + 141222 | Apple_HFS |

A/UX bzbs carry roles, not slice numbers, so A/UX disks exercise passes 2 and 5.

## Host adapter design

The Q800's on-board 53C96 has no bus-master DMA.
The driver is interrupt-driven with disconnect/reselect, and the CPU
moves data through the chip's DMA port in 256-byte blind chunks.

**Requests.** Each target 0–6 has a queue of `sdcom`s and at most one issued command
(`T_FREE`, `T_BUS` selecting or connected, `T_DISC` disconnected). One target is on the bus at a
time (`act`). On bus free the next target with queued work and nothing issued is selected,
round robin, so a disk can be selected while another is disconnected. Each target keeps a
current and a saved data pointer and count.

**Completion mode** (`ncr_intrmode`):

- 2 (default): interrupts, but `sdqueue` polls to completion until the 1 s watchdog has run
  once (early boot) and after a panic;
- 1: interrupts (polled after a panic);
- 0: always polled.

Polling runs at IPL 2 in the caller; requests queued meanwhile join the loop, which ends when
all queues are empty and no target is disconnected. In interrupt mode `p2int` acknowledges VIA2
IFR bit 3 and calls `ncr96intr`, which loops until STAT bit 7 stays clear. While a target is
connected it waits up to `ncr_spin` (100 µs) for the next interrupt, since most phase changes
follow within microseconds. The 1 s watchdog takes lost interrupts (`ncr_nlost`), resets the
bus when a command is older than `ncr_timo` (30 s, including disconnected time), and starts
queued work.

## Register-level sequence

Registers at 0x50F10000 + 16·n; pseudo-DMA port at +0x100, 16-bit. DREQ is bit 0 of a
**long** read at 0x50F03A00.

**Init** (first `sdopen` or `sdqueue`):

1. VIA2 IER = 0x08 (CB2 off).
2. CMD = 0x02 (reset chip), then 0x00.
3. CCF = 5, select timeout = 0xA4, CONF1 = 0x47 (ID 7, bus-reset interrupt off), CONF2 = 0,
   CONF3 = 0x04, sync offset = 0 (async), sync period = 5.
4. CMD = 0x03 (reset bus), then 0x00; settle `ncr_settle` ticks (sleeps in `sdopen`).
5. Read INTR if STAT bit 7 is set.
6. Unless `ncr_intrmode` = 0: VIA2 PCR CB2 = falling edge, IFR = 0x08, IER = 0x88.
7. Start the watchdog.

**Bus free** (`ncr_start`, only with no interrupt pending):

1. If a target is disconnected and reselection is not enabled: CMD = 0x01 (flush), CMD = 0x44
   (enable selection/reselection).
2. If a target has work: CMD = 0x01, SELID = target, select timeout = 0xA4, sync offset = 0.
3. FIFO ← IDENTIFY 0xC0 (disconnect allowed, LUN 0; 0x80 with `ncr_disc` = 0) and the CDB
   (6/10/12 bytes by group code).
4. CMD = 0x42 (select with ATN).

**Each interrupt:** read STAT, STEP & 7, then INTR (clears the interrupt).

- **Any state:**
  - INTR bit 7 (bus reset by another device): re-init, fail every issued command.
  - INTR bit 6 (illegal command): bus reset, re-init, fail every issued command.
  - INTR bit 2 (reselected; normally with FC, message-in, ACK held):
    1. A running selection lost: its request returns to the front of its queue.
    2. The FIFO holds the bus ID bits (0x80 plus the target's) and IDENTIFY. Read both,
       CMD = 0x01.
    3. Exactly one other ID bit must be set, else bus reset.
    4. Known nexus (target disconnected, LUN 0): restore saved pointers, CMD = 0x12.
    5. Unknown nexus: CMD = 0x1A (set ATN), CMD = 0x12, send ABORT (0x06) at message-out,
       expect bus free.
- **By state:**
  - After select, INTR bit 5 (step 0) = selection timeout: flush, fail (`okay = FALSE`,
    `ncr_nnosel`). Anything else means connected.
  - After ICCS, FC: FIFO holds status then message; take the status, handle the message.
  - After message-in, FC: one byte in the FIFO, ACK held; then CMD = 0x12 unless noted:
    - COMMAND COMPLETE, DISCONNECT: remembered for the following bus free.
    - SAVE DATA POINTER: saved := current. RESTORE POINTERS: current := saved.
    - REJECT, NOP, IDENTIFY: accepted.
    - Extended messages (e.g. SDTR): read to the end, CMD = 0x1A before 0x12, send MESSAGE
      REJECT. Anything else is rejected the same way.
  - After a DMA transfer command: see *Data*, then treat as bus service.
  - After a one-byte transfer: take the byte (data in), or count it if the FIFO is empty
    (data out; otherwise flush).
- **Bus free** (INTR bit 5 while connected), after CMD = 0x01: after DISCONNECT the target is
  disconnected with its saved pointers; otherwise the request completes, `okay` = "a status
  byte was received".
- **Bus service:** CMD = 0x01, then by STAT bits 0–2:
  - command: FIFO ← CDB, CMD = 0x10;
  - status: CMD = 0x11 (ICCS);
  - message out: FIFO ← pending message or NOP, CMD = 0x10;
  - message in: CMD = 0x10;
  - data in/out: see below. A phase against the request's direction, or data out beyond the
    request, resets the bus; excess data in is read a byte at a time and discarded.

**Data phase.** One transfer command per bus service:

- **Mode 2 (default), blind chunks:** while at least 256 bytes remain at an even address:
  1. TCL = 0x00, TCM = 0x01, CMD = 0x90 (DMA + transfer information).
  2. `ncr_blind`: wait for DREQ, move one word; wait for DREQ, move 127 words back to back
     (`movew` from/to 0x50F10100). Each wait ends on STAT bit 7 (phase change) or after 10⁶
     polls. The chunk runs under the `u_nofault` pad.
  3. On completion return; the chip interrupts when the target wants the next byte.
  4. DREQ missing or bus error: if the chip has interrupted, end-of-command handling sorts it
     out. Otherwise the CPU's position is (256 − TC) − FIFO count for reads, 256 − TC for
     writes, and the chunk is finished with the mode-1 loop.
- **Remainder** (< 256 bytes, odd address) and **mode 0**: one byte per command. Out: FIFO ←
  byte, CMD = 0x10. In: CMD = 0x10, take the byte at the interrupt.
- **Mode 1:** TC up to 0x8000 (even), CMD = 0x90. Reads move ⌊FIFO count/2⌋ words at a time,
  writes ⌊(16 − count)/2⌋, so no access can stall. Ends when all bytes have moved or the chip
  interrupts with less than a word in the FIFO. Unguarded by design.
- **Mode 3:** hook for a real-DMA backend (`ncr_dmaops`: AV Quadra PSC, Q900/950 PDS card).
  `start(pa, n, in)` before CMD = 0x90, `done(took, in)` at the next interrupt. No backend
  exists; with `ncr_dmaops` unset it behaves as mode 2.
- **End of a DMA command** (next interrupt). TC counts bytes entering the FIFO (from the bus on
  reads, from the CPU on writes):
  - took = requested − TC (0 when STAT bit 4, count zero, is set);
  - reads: remaining FIFO bytes are read through the FIFO register; position += took;
  - writes: FIFO bytes never reached the target: position += took − FIFO count, CMD = 0x01.

  TC is read TCM, TCL, TCM until both TCM reads agree.

**Completion:** target state is cleared, then `cp->intr(cp)` runs (`dd`'s `ihandle`, which may
queue the next request at once, even on the same `sdcom`); then the next selection starts.

**Abort:** CMD = 0x03, wait 250 ms, re-init, fail every issued command (connected or
disconnected; `okay = FALSE`, so `dd` sets `B_ERROR`). Queued requests stay.

## Bus errors on the 68040: the pad

- 0x50F10100 is mapped by page tables (`mac_iomap` PTEs `pa|0xC1`, supervisor, noncacheable
  serialized); DTT0/DTT1 do not cover it. A stalled cycle gives a TEA access error, format-7
  frame, SSW ATC = 0.
- Vector 2 → `nullvect` → `nullvect_orig` → `ktraps` → `k_trap`, which dispatches on the
  vector only.
- With `u.u_nofault` (`u_caddrflt`, u+0x374) armed, `k_trap`:
  1. saves and clears it while resolving;
  2. treats TM = 5 as a kernel access and calls `krnxmemflt`; `as_segat(&kas, 0x50F10100)` is
     NULL, so the fault is unresolved;
  3. restores `u_nofault` and writes it into the frame PC;
  4. returns nonzero, so `ktraps` goes to `stkclear`: the format-7 frame is replaced by a
     format-0 frame and `rte` runs the pad with d0–d7/a0–a6, SSP and SR as at the fault.
- **Writes are not replayed:** pending write-backs of the dropped frame are discarded
  (`wb040_replay` runs only for resolved faults), so a faulted port write never repeats as a
  stray DACK cycle. The console may print "unresolved fault dropped a pending write-back"
  (capped at 8), and `krnxmemflt` prints `DBG krnxflt FAILEXIT w=2 va=50f10100 …` (capped at 8
  per boot).
- `hardbus` and its 0x40000000–0x7FFFFFFF band serve user-mode faults only.
- **`ncr_blind`'s pad:**
  - is a label in the same function and restores registers from %fp;
  - restores the outer `u_nofault` (possibly an interrupted copyin/copyout's), since `k_trap`
    leaves the pad armed;
  - takes the count from the chip, since a write fault is reported after later instructions
    may have run;
  - compares SP with the value saved when armed and panics on mismatch, so a fault in a nested
    interrupt handler (IPL > 2) cannot return into the wrong frame.
- Without the pad, a supervisor bus error panics with "KERNEL FAULT"; mode 1 is safe only
  because it never stalls.

## Compiler trap

`AMIX_KERNEL_CFLAGS` uses `-traditional`, which affects every C file built with the AMIX cross
gcc:

- **`volatile` is erased:** `sys/types.h` defines it empty for non-ANSI compilers. Hardware
  pointers use `__volatile__`.
- **`sys/inline.h` spl functions vanish:** their `asm volatile(...)` becomes plain `asm`, which
  gcc drops when the result is unused, leaving no SR writes. `macspl.h` provides
  `__volatile__` versions; `splscsi` never lowers the level.

## Sources of facts

- **Chip:** the published NCR 53C9x register/command set (NetBSD `ncr53c9xreg.h` names); NetBSD
  `ncr53c9x.c`, `lsi64854.c` and `arch/mac68k/obio/esp.c` for facts only (TC counts bytes into
  the FIFO; reselection bytes stay in the FIFO with writes locked until INTR is read; Q800
  DREQ in VIA2 IFR bit 0; 16-bit port accesses move two bytes; stalls end in bus errors;
  PCR = 0x22 with IER bit 3).
- **Partition map:** public Inside Macintosh DDM/partition map layout; A/UX's shipped headers
  `apple/bzb.h`, `apple/dpme.h`, `sys/gdisk.h`. The pass structure follows NetBSD
  `disksubr.c`'s role idea.
- **AMIX interfaces:** the link kit's `sd.h`, `dd.c`, `scsi.c`, `sdpart.c`, `a3091.c`,
  `physdsk.c` and `usr/include`.

## Build and test

- `sh kernel/mac/scsi/build.sh [outdir]` builds `macscsi.o`. Its imports (`delay`, `delayus`,
  `iodone`, `lbolt`, `panic`, `panicstr`, `printf`, `sleep`, `timeout`, `u`, `wakeup`) are
  defined in the base or the Mac layer.
- `sh kernel/mac/scsi/trial-link.sh WORKDIR` links the kernel in a scratch tree and runs the
  image checks: all `sd*` overrides bound, nothing unresolved, validator clean.
- Worth checking in the disassembly: SR writes present in `splscsi`/`splrestore`; register
  polls re-read the chip; discarded INTR reads kept; `ncr_tc` re-reads TCM; `ncr_blind` has
  128 `movew` per direction and arms/restores u+0x374 on both exits; `p2int` calls
  `ncr96intr`.
- `python3 kernel/mac/scsi/test/run.py WORKDIR [AUX_DISK_HEAD]` tests `apm.c` on synthetic
  images (A/UX roles, explicit slices including one above 7, pdisk names, lower-case type,
  skipped driver/free entries, overflow past s7, blank disk, 2048-byte blocks, no DDM,
  truncated image) and optionally the A/UX 3.1 disk's map.
- `sh kernel/mac/scsi/test/ncrtest.sh WORKDIR [-v] [SCENARIO]` compiles `ncr96.c` unchanged
  (`-DNCR_HOST` swaps register macros and headers) against `ncrsim.c`: a 53C96 model (FIFO,
  16-bit TC, INTR/STAT/STEP, reselection FIFO lock, a port drainable after a read's phase
  change), disks at IDs 0–2, a VIA2 CB2 latch, simulated time, and a C twin of `ncr_blind`
  where a port cycle stalled over 20 µs is a bus error. Scenarios cover: all four data modes;
  TC interrupt before the FIFO drains; remainders and read overrun; IDENTIFY without
  disconnect; disconnects after the command, mid-data with and without SAVE DATA POINTER, and
  inside a blind chunk with FIFO residue in both directions; interleaved targets; reselection
  beating our selection; selection timeout; injected bus errors; long target stalls; lost
  interrupts; automatic-mode transitions; SDTR rejection. Removing any of the pointer restore,
  SAVE DATA POINTER handling, backoff, ENABLE RESELECTION, write-FIFO residue or read-FIFO
  drain makes a scenario fail.

## Open hardware questions

1. **Chip clock.** A/UX says 25 MHz (CCF 5, select timeout 0xA4); NetBSD gives 16.5 MHz for
   0x10000-offset machines. CCF 5 suits either. Selection timeouts should take about 250 ms
   with no timing errors.
2. **VIA2 CB2 edge.** One interrupt per chip event, `ncr_nlost` near 0; check PCR as the ROM
   leaves it.
3. **DREQ** is bit 0 of the long at 0x50F03A00. If it never shows, `ncr_nnodreq` climbs and
   every chunk falls back to mode 1.
4. **Blind chunks:** two bytes per 16-bit access in SCSI order (compare a block read in modes
   0, 1, 2); the glue stalls each cycle until the FIFO is ready, both directions.
5. **Stall → bus error:** a timed-out stall must end in TEA, not a retry, and reach the pad
   (`ncr_nfault`, console `FAILEXIT … va=50f10100`); the recovery arithmetic must hold (force
   with a disconnect inside a chunk or a slow device); "dropped a pending write-back" should
   not grow unexpectedly.
6. **TC and FIFO after a phase change:** can reads still drain the port, with remaining bytes
   through the FIFO register; are unsent write bytes `FFLAG`? A wrong count corrupts data on
   disconnects inside a chunk; work around with `ncr_disc` = 0 or `ncr_pdmamode` = 1.
7. **Disconnect/reselect:** the internal drive accepts IDENTIFY 0xC0; reselection leaves
   exactly 2 FIFO bytes; 0x44 followed at once by a selection works; a reselection beating our
   selection cancels it (INTR bit 2, no illegal command). Watch `ncr_ndisc`/`ncr_nresel`
   during a large copy.
8. **ICCS** gives FIFO count 2; the final disconnect follows MESSAGE ACCEPTED.
9. **Bus-reset settle:** 2 s may be short for a spinning-up drive; `dd` retries one unit
   attention, not a not-ready.
10. **Cold polling** at IPL 2 before the first watchdog tick (and after a panic) delays VIA1
    clock ticks.
11. **Partition read at open** uses `dd`'s strategy with a static buffer; a drive with
    non-512-byte blocks or no map should still open as s0.

## Limits

- One command per target; no tagged queueing; no synchronous transfer (SDTR rejected).
- LUN 0 and card 0 only (the Q900/950 second bus and DAFB DREQ are unhandled).
- 512-byte blocks only.
- A bus reset fails every issued command, including disconnected ones on other targets; queued
  requests survive.
- `ncr_timo` (30 s) counts disconnected time; long tape operations need it raised.
- Only the blind chunk is guarded; mode 1 relies on never stalling.
- A bus error in a nested interrupt handler during a chunk panics.
- The chip is initialised on first use; a second opener during the settle sleep can start I/O
  early, and `dd`'s retry covers the unit attention.
- Real-DMA backends are a hook only.
