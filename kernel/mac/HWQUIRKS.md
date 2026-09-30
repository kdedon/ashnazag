# Real Quadra 800 quirks QEMU may not model

Our Mac drivers compared with NetBSD mac68k and Linux m68k (`ref/netbsd`, `ref/linux`). State 2026-09-30, static review only. Ranked by risk on real hardware.

| # | Risk | Area | NetBSD / Linux | Ours | Change |
|---|---|---|---|---|---|
| 1 | high | DAFB CLUT | DAFB cannot address single entries: write 0 to the index, reload from entry 0 (Linux `macfb.c` `dafb_setpalette`; NetBSD `grf_obio.c` `dafb_set_mapreg`) | `display/ds.c` `ds_clutload` writes `lo` to CLUTADDR | always write 0, load 0..hi-1 from the shadow table |
| 2 | high | ADB, PB3 low after command byte | a pending auto-poll reply is being delivered; our command was not sent; switch SR to input, record as auto-poll data, retry the request (Linux `via-macii.c` ~417-445) | `adb/adb.c` S_CMD takes it as SRQ and continues | go to S_AUTO with the last poll talk, SR in, EVEN; requeue the request |
| 3 | med | ADB, SRQ at data byte 1 | ODD low at byte 1 = SRQ; keep reading (Linux `via-macii.c` ~497) | ends the reply at byte 1; timed-out Talk collects no SRQ | set `adb_srq`, keep reading; toggle once more on timeout |
| 4 | med | ADB, IDLE before CMD | NetBSD holds IDLE ≥150 µs; Linux skips IDLE | IDLE then CMD within ~2 VIA cycles | drop IDLE (Linux) or `adb_delay(150)` |
| 5 | med | VIA2 slot line held low | serve all low PA lines, re-ack CA1 until clear (Linux `mac/via.c`) | `macintr.s`: other slot low → count and return; CA1 stays low, SONIC/VBL stop | mask stuck slots (or poll SONIC/VBL from the tick); log the slot |
| 6 | med | IPL 3/5/6 | spurious/off-switch levels counted | `macintr.s` panics | count, rate-limited message, return |
| 7 | med | SONIC LCAM vs TXP | wait for TXP clear before LCAM (Linux `sonic.c`) | `sonic/sonic.c` `sn_filter` stops RX only | `sn_crwait(CR_TXP)` before CAM load, at splnet |
| 8 | med | ESP bus reset on error path | 3 s settle after every reset (Linux `esp_scsi.c`) | 2 s at init, 250 ms in error recovery (`scsi/ncr96.c`) | use the init settle (≥2 s) there too |
| 9 | low-med | ESP chip reset | DELAY(500) after RSTCHIP+NOP before config (NetBSD `ncr53c9x.c`) | config written at once | `delayus(500)` after NOP |
| 10 | low-med | DAFB register writes | `nop` between LUT writes (Linux) | DTT1 covers 0xF9800000 as non-serialized; back-to-back byte writes | `nop` after each LUT/INTCLEAR/INTMASK write, or a serialized page like `PG_IO` |
| 11 | low | SONIC TXER | reissue TXP on the aborted descriptor (Linux) | aborted descriptor counted done, TXP if more pending | check CTDA after TXER on hardware |
| 12 | low | SONIC bus retry | BR enabled and counted (Linux) | BR not in IMR | enable and count; only sign of DMA-glue errors |
| 13 | low | ESP FIFO before data-out | flush before each data-out DMA (Linux `mac_esp.c`) | none | `C_FLUSH` when writing |
| 14 | low | VIA2 CA2 (SCSI DREQ) | PCR = 0x22 (Linux) | only CB2 bits set, CA2 left from ROM | set CA2 to 001 too |
| 15 | low | ESP CFG1 SRR | reset interrupt enabled (NetBSD) | 0x47 hides bus resets from other initiators | 0x07 if external resets matter |
| 16 | low | Level 7 (interrupt button) | — | Amiga level-7 handler still linked | add a Mac `p7int` that prints and returns |
| 17 | low | VIA2 PB1 /BusLock, T1/T2, ACR | Linux sets PB1 output high, clears VIA2 timers, ACR &= ~0xC3 | untouched (ROM values) | set in `via_quiet`; matters for boots without the ROM's setup |
| 18 | low | Power off | VIA2 PB2 low (Linux `misc.c`, ADB-II machines) | halt prints only | drive PB2 low, spin |
| 19 | low | Delays | calibrated delay loops | `delayus`/`adb_delay` count VIA reads (1.3–2.5 µs each on hardware) | fine on hardware (long); QEMU runs them short, so it proves no timing margin |
| 20 | info | ESP clock | both use 16.5 MHz on the Q800, CCF 4 | CCF 5, SELTO 0xA4 from A/UX (comment says 25 MHz) | keep values; fix the comment; re-derive before enabling sync |
| 21 | info | XPRAM address byte | Linux ORs 01, NetBSD sends 00 | 00 | change only if reads fail |

Already matching: I/O page 0x50F00000 is noncacheable serialized (`PG_IO` 0xC1); VIA layout, level mapping, T1 60 Hz tick and edge acks; ESP interrupt ack order, reselection handling, 16-bit PDMA with odd-byte/residue handling and bus-error landing pad; SONIC DCR/DCR2 (NOTES says 0x803A, code 0x8039: doc typo), MAC PROM bit-reversal, RBE/RDE recovery, cache-inhibited descriptor pool; SCC offsets, 3.6864 MHz clock, reset and recovery; RTC bit-bang, write-protect, seconds double-read; reboot via ROM+0xA after MMU/caches off.
