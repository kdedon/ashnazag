# Real Quadra 800 quirks QEMU may not model

Our Mac drivers compared with NetBSD mac68k and Linux m68k (`ref/netbsd`, `ref/linux`). Static review; ranked by risk on real hardware. Fixed items are listed in `bootdiag/NOTES.md` and `adb/NOTES.md`.

| # | Risk | Area | NetBSD / Linux | Ours | Change | Status |
|---|---|---|---|---|---|---|
| 1 | high | DAFB CLUT | DAFB cannot address single entries: write 0 to the index, reload from entry 0 (Linux `macfb.c` `dafb_setpalette`; NetBSD `grf_obio.c` `dafb_set_mapreg`) | `display/ds.c` `ds_clutload` writes `lo` to CLUTADDR | always write 0, load 0..hi-1 from the shadow table | open |
| 2 | high | ADB, PB3 low after command byte | a pending auto-poll reply is being delivered; our command was not sent; switch SR to input, record as auto-poll data, retry the request (Linux `via-macii.c` ~417-445) | `adb/adb.c`: a low PB3 after the command byte is a pending auto-poll reply; it is read and the command resent | — | fixed (adb/NOTES.md) |
| 3 | med | ADB, SRQ at data byte 1 | ODD low at byte 1 = SRQ; keep reading (Linux `via-macii.c` ~497) | `adb/adb.c`: byte 1 low is SRQ and the read continues | — | fixed (adb/NOTES.md) |
| 4 | med | ADB, IDLE before CMD | NetBSD holds IDLE ≥150 µs; Linux skips IDLE | `adb_delay(150)` before a command that follows a dataless one | — | fixed (adb/NOTES.md) |
| 5 | med | VIA2 slot line held low | serve all low PA lines, re-ack CA1 until clear (Linux `mac/via.c`) | `macintr.s`: a stuck slot is served, CA1 off, port A kept in `mac_slotstuck`; the tick serves SONIC/VBL and re-enables CA1 | — | fixed (bootdiag/NOTES.md) |
| 6 | med | IPL 3/5/6 | spurious/off-switch levels counted | `macintr.s`: counted in `mac_spurious[n]`, first one printed, panic only on a storm | — | fixed (bootdiag/NOTES.md) |
| 7 | med | SONIC LCAM vs TXP | wait for TXP clear before LCAM (Linux `sonic.c`) | `sonic/sonic.c` `sn_filter` stops RX only | `sn_crwait(CR_TXP)` before CAM load, at splnet | open |
| 8 | med | ESP bus reset on error path | 3 s settle after every reset (Linux `esp_scsi.c`) | `ncr96.c`: error-path reset settles `ncr_settle` (2 s) | — | fixed (bootdiag/NOTES.md) |
| 9 | low-med | ESP chip reset | DELAY(500) after RSTCHIP+NOP before config (NetBSD `ncr53c9x.c`) | `ncr96.c`: `delayus(500)` after chip reset | — | fixed (bootdiag/NOTES.md) |
| 10 | low-med | DAFB register writes | `nop` between LUT writes (Linux) | DTT1 covers 0xF9800000 as non-serialized; back-to-back byte writes | `nop` after each LUT/INTCLEAR/INTMASK write, or a serialized page like `PG_IO` | open |
| 11 | low | SONIC TXER | reissue TXP on the aborted descriptor (Linux) | aborted descriptor counted done, TXP if more pending | check CTDA after TXER on hardware | open |
| 12 | low | SONIC bus retry | BR enabled and counted (Linux) | BR not in IMR | enable and count; only sign of DMA-glue errors | open |
| 13 | low | ESP FIFO before data-out | flush before each data-out DMA (Linux `mac_esp.c`) | `ncr96.c`: FIFO flushed before each data-out DMA | — | fixed (bootdiag/NOTES.md) |
| 14 | low | VIA2 CA2 (SCSI DREQ) | PCR = 0x22 (Linux) | `ncr96.c`: PCR CA2 and CB2 set | — | fixed (bootdiag/NOTES.md) |
| 15 | low | ESP CFG1 SRR | reset interrupt enabled (NetBSD) | 0x47 hides bus resets from other initiators | 0x07 if external resets matter | open |
| 16 | low | Level 7 (interrupt button) | — | Amiga level-7 handler still linked | add a Mac `p7int` that prints and returns | open |
| 17 | low | VIA2 PB1 /BusLock, T1/T2, ACR | Linux sets PB1 output high, clears VIA2 timers, ACR &= ~0xC3 | untouched (ROM values) | set in `via_quiet`; matters for boots without the ROM's setup | open |
| 18 | low | Power off | VIA2 PB2 low (Linux `misc.c`, ADB-II machines) | halt prints only | drive PB2 low, spin | open |
| 19 | low | Delays | calibrated delay loops | `delayus`/`adb_delay` count VIA reads (1.3–2.5 µs each on hardware) | fine on hardware (long); QEMU runs them short, so it proves no timing margin | open |
| 20 | info | ESP clock | both use 16.5 MHz on the Q800, CCF 4 | CCF 5, SELTO 0xA4; comment says 16.5 MHz | — | fixed (bootdiag/NOTES.md) |
| 21 | info | XPRAM address byte | Linux ORs 01, NetBSD sends 00 | 00 | change only if reads fail | open |

Already matching: I/O page 0x50F00000 is noncacheable serialized (`PG_IO` 0xC1); VIA layout, level mapping, T1 60 Hz tick and edge acks; ESP interrupt ack order, reselection handling, 16-bit PDMA with odd-byte/residue handling and bus-error landing pad; SONIC DCR/DCR2 (`sonic/NOTES.md` says 0x803A, code 0x8039), MAC PROM bit-reversal, RBE/RDE recovery, cache-inhibited descriptor pool; SCC offsets, 3.6864 MHz clock, reset and recovery; RTC bit-bang, write-protect, seconds double-read; reboot via ROM+0xA after MMU/caches off.
