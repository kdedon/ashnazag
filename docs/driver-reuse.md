# Driver reuse: NetBSD and Linux sources for our 68k hardware

State 2026-09-30. Reference trees (local only, sparse, depth 1): `ref/netbsd` (NetBSD src HEAD: `sys/arch/{mac68k,atari,m68k}`, the `sys/dev/ic`, `sys/dev/scsipi` headers and `sys/dev/adb` files those ports use) and `ref/linux` (torvalds HEAD: `arch/m68k`, `drivers/macintosh`, the Mac/Atari SCSI, SONIC, MACE, fbdev, pata_falcon, atakbd and pmac_zilog files). Real-Q800 discrepancies in our current drivers: [kernel/mac/HWQUIRKS.md](../kernel/mac/HWQUIRKS.md).

## Licences

| Source | Licence | Use |
|---|---|---|
| NetBSD, 2-clause (TNF, most `dev/ic`, `if_mc`, `iop.c`, `psc.c`, atari `ncr5380.c`, `kbd.c`, `grfabs_*`) | BSD-2 | copy with notice |
| NetBSD, UCB 4-clause (`z8530sc.c`, `z8530tty.c`, atari `zs.c`) | clause 3 rescinded by UCB 1999 | copy with notice |
| NetBSD, individual-author 4-clause (`adb_direct.c`, `pm_direct.c`, `akbd.c`, `ams.c`, `adb_kbd.c`, `macfb.c`, `grf_obio.c`, `asc.c`, `esp.c`, `nubus.c`, `via.c`, `pram.c`, `pramasm.s`, atari `ms.c`, `ser.c`, `ncr5380sbc.c`, Galbavy part of `ncr53c9x.c`) | advertising clause; GPL-incompatible | keep as a separate BSD module, get a relicence from the author, or use as reference and rewrite |
| Linux | GPL-2 only | fine for the GPL-2 repo, conflicts with PLAN's "our modules stay MIT/BSD"; reference for quirks unless that rule is relaxed |

Check each file's header before copying; several mix holders.

## Reuse map

Effort: S ≤ 2 days, M ≤ 1 week, L > 1 week, for an SVR4 DDI driver on the shim below. "HW" = register-level code worth keeping; "FW" = NetBSD/Linux framework glue to replace.

### Macintosh

| Hardware (machines) | Best source | Lines | HW part / FW part | Effort |
|---|---|---|---|---|
| 53C96 ESP + pseudo-DMA (Q650/700/800/900/950) | NetBSD `dev/ic/ncr53c9x.c` + `arch/mac68k/obio/esp.c` (PDMA, DRQ via VIA2) | 2985 + 1202 | chip state machine + PDMA / `scsipi_xfer`, callouts, autoconf | M (ours exists; mine for quirks) |
| 53C96 via PSC DMA (Q660AV/840AV) | NetBSD `esp.c` (`esp_av_*`, PSC channels) + `mac68k/psc.c`; Linux `mac_esp.c` for PSC notes | 1202 + 490 | PSC DMA setup/stop / same as above | M |
| NCR 5380 (IIci, IIfx, LC, LC II/III, Q605/610/630, PowerBooks) | NetBSD `dev/ic/ncr5380sbc.c` + `mac68k/dev/sbc.c`, `obio/sbc_obio.c` (PDMA, DRQ, bus-error recovery); Linux `mac_scsi.c`+`NCR5380.c` for PDMA quirks | 2613 + 832 + 314 | phase engine + PDMA / `scsipi` | M |
| SONIC DP83932 (Q650/700/800/900/950, some LC comm-slot, NuBus) | NetBSD `dev/ic/dp83932.c` + `mac68k/dev/if_sn.c`, `obio/if_sn_obio.c` | 1275 + ~600 | descriptor rings / mbuf, `bus_dma`, `ifnet` | exists (ours); mine for errata |
| MACE Am79C940 (Q660AV/840AV) | NetBSD `mac68k/dev/if_mc.c` + `obio/if_mc_obio.c` (PSC DMA); Linux `macmace.c` | 739 + 433 | register init, PSC rings / mbuf, `ifnet` | M (reuse our `sndlpi.c` DLPI layer) |
| ADB via VIA1, Mac II protocol (Q700/800/650, IIci, IIvx, LC III, Q605/610) | NetBSD `mac68k/dev/adb_direct.c` (`adb_intr_II`); Linux `via-macii.c` (cleaner, 559 lines) | 3011 / 559 | state machine / event delivery | exists (ours) |
| ADB via Egret/Cuda (IIsi, LC, LC II, Q660AV/840AV, Q630) | NetBSD `adb_direct.c` (IISI, CUDA paths); Linux `via-cuda.c` | — / 802 | transceiver protocol; also RTC/PRAM/reboot on these machines | M |
| IOP: ADB and SCC/SWIM (IIfx, Q900/950) | NetBSD `mac68k/mac68k/iop.c` + `adb_iop_*` in `adb_direct.c`; Linux `arch/m68k/mac/iop.c`, `drivers/macintosh/adb-iop.c` | 455 / 590 + 297 | IOP message mailbox, SCC bypass mode / none | M |
| OSS interrupt controller (IIfx) | NetBSD `via.c` OSS path; Linux `arch/m68k/mac/oss.c` | 188 | all HW | S |
| PSC interrupt/DMA controller (Q660AV/840AV) | NetBSD `mac68k/psc.c`; Linux `arch/m68k/mac/psc.c` | 490 / 168 | all HW | S |
| RBV / V8 (IIci, IIsi, LC) interrupt + video | NetBSD `via.c`, `obio/grf_obio.c`; Linux `mac/via.c`, `macfb.c` | 512 / 654 | all HW | S |
| Z85C30 SCC (all) | NetBSD `dev/ic/z8530sc.c`, `z8530tty.c`, `mac68k/dev/zs.c`; Linux `pmac_zilog.c` | 442 + 1751 + 1045 | register sequencing / BSD tty | exists (ours) |
| ASC sound (all non-AV) | NetBSD `obio/asc.c` (bell, raw FIFO), `obio/ascaudio.c` (audio(4), EASC) | 404 + 1123 | FIFO/IRQ / audio(4) | M (STREAMS audio device) |
| AV sound (Singer/DSP3210) | none | — | — | L (own) |
| SWIM/IWM floppy | NetBSD `obio/iwm_fd.c` + `iwm.s` (IWM mode only: SE/30, II, IIx, IIcx, IIci, LC); Linux `drivers/block/swim.c` (not fetched) | 1950 + 1516 | GCR in asm / disk(9) | L; SWIM in native (ISM) mode on Quadras only in Linux `swim.c`; IOP SWIM (IIfx, Q900/950) in neither |
| Onboard video: DAFB (Q700/800/900/950), CIVIC (AV), V8/RBV, Valkyrie | NetBSD `obio/grf_obio.c`; Linux `macfb.c` (CLUT writers per chip) | 510 / 899 | CLUT + mode regs / grf, fbdev | S each (our `fbprobe.c` takes booter params) |
| NuBus video | NetBSD `nubus/grf_nubus.c` + `nubus.c` (declaration ROM parse, per-card CLUT) | 765 + 844 | sResource parse, CLUT pokes / grf | M |
| RTC/PRAM | NetBSD `mac68k/pram.c`, `pramasm.s`; Linux `mac/misc.c` (VIA, Egret, Cuda, PMU variants) | 229 + 417 / 666 | bit-bang / none | exists (ours); S per transceiver |

### Atari TT / Falcon

| Hardware | Best source | Lines | HW part / FW part | Effort |
|---|---|---|---|---|
| MFP 68901 (IRQ, timers, TT second MFP) | NetBSD `atari/atari/intr.c`, `dev/clock.c`; Linux `arch/m68k/atari/ataints.c`, `time.c` | 325 + 589 | all HW | S |
| MFP serial | NetBSD `atari/dev/ser.c` (4-clause) | 1489 | USART regs / BSD tty | S–M (reuse our SCC STREAMS tty upper half) |
| SCC 85C30 (TT, Falcon, MegaSTE) | NetBSD `atari/dev/zs.c` + `dev/ic/z8530*` | 1340 | same chip as Mac | S (port our `scc.c`: new base, PCLK 8 MHz, IRQ vector) |
| IKBD 6301 keyboard/mouse | NetBSD `atari/dev/kbd.c`, `ms.c`; Linux `atari/atakeyb.c`, `input/keyboard/atakbd.c` | 924 + 438 | ACIA packet parser / wscons | S |
| TT SCSI 5380 (TT DMA) and Falcon 5380 (ST-DMA, shared lock) | NetBSD `atari/dev/ncr5380.c` + `atari5380.c`, `dma.c`; Linux `atari_scsi.c` + `NCR5380.c`, `atari/stdma.c` | 2068 + 1159 + 270 | DMA engines, ST-DMA arbitration / `scsipi` | M (shares 5380 core with Mac) |
| Falcon IDE | NetBSD `atari/dev/wdc_mb.c` (needs wdc/ata, large); Linux `pata_falcon.c` (libata) | 285 / 240 | byte-swapped data bus, ST-DMA lock / ATA framework | S: own polled PIO ATA using their quirks |
| ACSI | none in either (Linux `acsi.c` removed in 2.6) | — | — | M (own; ST-DMA shared with Falcon SCSI) |
| Video: TT shifter, Falcon Videl | NetBSD `atari/dev/grfabs_tt.c`, `grfabs_fal.c`; Linux `atafb.c` | 422 + 637 / 3474 | mode tables, palette / grf, fbdev | S–M |
| Floppy WD1772 | NetBSD `atari/dev/fd.c` | 1342 | FDC + ST-DMA / disk(9) | M |
| NVRAM/RTC MC146818 | NetBSD `atari/dev/nvram.c`, `clock.c`; Linux `atari/nvram.c` | 207 / 274 | all HW | S |
| YM2149 (floppy select, parallel strobe) | NetBSD `atari/dev/ym2149.c` | 58 | all HW | S |
| CT60 68060 | no CT60 driver in either; generic 060 in NetBSD `arch/m68k/060sp` (Motorola FPSP/ISP) and `locore.s`; `ref/ct60tos` for TT0/TT1, SDRAM map, PCR | — | CPU setup only | S (AMIX 060 port already covers the CPU) |

## Shim: NetBSD chip code on AMIX SVR4

Keep NetBSD's chip files (`dev/ic/*`, attachments' register code) nearly unchanged; compile them against a small `nbshim.h` + `nbshim.c`.

| NetBSD API | AMIX SVR4 mapping |
|---|---|
| `bus_space_{read,write}_{1,2,4}`, `_multi_`, `bus_space_map` | inline `__volatile__` loads/stores; handle = VA (Mac I/O is VA = PA uncached, Atari I/O via the kernel map); `bus_space_barrier` = `nop` on 040 |
| `bus_dmamap_*`, `bus_dmamem_*` | `kmem_alloc` + `vtop`; one segment per page; `sync` = `cpushl`/`cinv` on 040/060, nothing on 030 (supervisor D-cache off); Atari ST-RAM allocator for DMA below 16 MB |
| `splbio`/`splnet`/`splserial`/`splx` | `macspl.h` wrappers (Mac IPL 2 / 2 / 4; Atari MFP 6) |
| `mutex_enter/exit`, `mutex_init` | spl raise/restore (uniprocessor) |
| `callout_reset/stop` | `timeout`/`untimeout` (ticks at `HZ` 60) |
| `delay`/`DELAY` | calibrated `delayus` |
| `tsleep`/`wakeup` | `sleep`/`wakeup` |
| `malloc`/`kmem_*` | `kmem_alloc`/`kmem_free` |
| `device_private`, `device_xname`, `config_found`, `aprint_*` | static softc, fixed names, no autoconf, `printf`/`cmn_err` |
| `scsipi_xfer`, `scsipi_done`, `scsipi_channel_*` | adapter to AMIX `dd`'s `sd.h` layer (`sdqueue` → build xfer, completion → `iodone`) |
| mbuf (`MGETHDR`, `m_freem`, `m_copydata`), `ifnet`, `ether_*`, `bpf_mtap` | adapter to `mblk_t` (`allocb`/`freemsg`) under our DLPI module (`sndlpi.c` pattern); bpf dropped |
| `ttyinput`, `tty_*` (z8530tty, ser) | not used: keep only chip code, reuse our STREAMS tty upper half (`scc.c`) |
| `rnd_*` | no-op |

Rules: one shim for all ported drivers; no NetBSD autoconf or `ifnet`; each driver a DLM module.
