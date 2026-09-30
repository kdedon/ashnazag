# Quadra 800 platform: hardware and driver specification

Specification for the Mac platform module of the AMIX-based kernel, first target the Macintosh Quadra 800 (68040, 33 MHz, $067C ROM): the hardware, how A/UX 3.1 drives it (derived from the shipped kernel binary), and what the new kernel has to do.

**Sources and citation keys**

| Key | Source |
|---|---|
| **[AUX]** | A/UX 3.1 `/unix` (COFF, unstripped). Addresses are A/UX kernel addresses. Low-level code is in section `pstart` (0x54000–0x5bd90, physical = virtual); drivers are in `.text` (0x10000000). Disassembly: `tools/coffdump.py`. Data words: `tools/auxpeek.py <coff> <addr> <len> [b\|w\|l]`. |
| **[HDR]** | Shipped A/UX headers: `sys/uconfig.h` (addresses, `struct c94info`, `struct onboard`), `sys/via6522.h`, `sys/obvideo.h` (DAFB), `sys/module.h` (`struct kernel_info`). |
| **[NBSD]** | NetBSD/mac68k: `sys/arch/mac68k/mac68k/{machdep.c,intr.c,via.c,pramasm.s}`, `include/viareg.h`, `dev/adb_direct.c`, `obio/if_sn_obio.c`. |
| **[PUB]** | Public documentation: *Guide to the Macintosh Family Hardware* (2nd ed.), *Macintosh Quadra 800 / Centris 650 / 610 Developer Notes*, NCR 53C96, Zilog Z85C30, Synertek/Rockwell 6522, National DP83932 datasheets. Not checked page by page; see *(verify)* marks. |

Marks: *(uncertain)* = inference not fully confirmed; *(verify)* = check on hardware or in the named document before relying on it.

## 1. Machine identification

### 1.1 How A/UX learns the machine type

- The A/UX booter (the Mac-side "A/UX Startup" application) enters the kernel at `_start` (0x54000) with `d0` = `'Smr\1'` (0x536d7201) and `a0` = pointer to `struct kernel_info` [HDR `sys/module.h`]. `_start` copies `si[3]` (text/data/bss `{vstart, pstart, size}`, at +0x8c) to `sectinfo` and the word `machine_type` (+0xb0) to `machineID` (0x5afa6) [AUX `_start`]. Without the magic, `kernelinfoptr` defaults to 0x400 (old booter).
- `machineID` is the Gestalt machine type **minus 2**. Checked against the CPU clocks that `boardinit` stores: 9 = IIci (25 MHz), 11 = IIfx (40), 16 = IIsi (20), 18 = Quadra 900 (25), 20 = Quadra 700, 24 = Quadra 950 (33), 28 = Centris 650 (25), **33 = Quadra 800 (33)**, 34 = Quadra 650 (33), 42 = IIvi (default 16), 43 = Performa 600 (32), 46 = IIvx (32), 50 = Centris 610 (20), 51 = Quadra 610 (25), 57 = Gestalt 59 *(unidentified; 40 MHz, same "Wombat" branch)*.
- `boardinit` (0x583de) calls `setupProductInfo`, then switches on `machineID − 4` through a 54-entry word table at 0x5882a. IDs 4–7 (II, IIx, IIcx, SE/30) take the defaults; IDs outside the table panic "unsupported motherboard type".
- The kernel does not check the ROM version to identify the machine. `setupProductInfo` (0x589ee) reads `RomVersion` = word at ROM+8 (0x40800008). If it is ≥ $067C, it copies 0x34 bytes of the ROM's ProductInfo record (pointer from Mac low memory `UnivInfoPtr` $DD8) into the kernel (0x5b684) and relocates its 7 internal pointers. `get_rbv_info` takes the built-in video base and slot from ProductInfo's video-info record (ProductInfo + [ProductInfo+8]: +4 base, +0xc slot).

### 1.2 The Quadra 800 branch ("Wombat" class: IDs 28, 33, 34, 50, 51, 57)

`boardinit` 0x5868a–0x587b0 sets [AUX; field names from `struct onboard`, `struct c94info` in HDR `sys/uconfig.h`]:

| Variable (A/UX address) | Q800 value | Meaning |
|---|---|---|
| `cpuspeed` (0x5b3ba) | 33 (0x21) | MHz |
| `onboard.sound` (0x5b3ce) | 2 | `batmanSoundHW` |
| `onboard.egret` (0x5b3cf) | 0 | **no Egret/Cuda**: ADB and RTC are VIA-based |
| `onboard.iop` (0x5b3d0) | 0 | no IOPs; SCC and SWIM directly addressed |
| `onboard.keysw` (0x5b3d1) | 0 | no keyswitch |
| `onboard.nc94` / `nc94_mother` (0x5b3d2/5) | 1 / 1 | one 53C96 on the motherboard |
| `onboard.memctrl` (0x5b3d3) | 2 | `MemCDJ` (djMEMC) |
| `onboard.ether` (0x5b3d4) | 1 | `OBEtherSonic` |
| `onboard.iobusclock` (0x5b3d6) | 0x1f | *(meaning unclear; only compared with 0x18 in s2_chipsubs floppy code)* |
| `iwm_addr` | 0x5001E000 | SWIM |
| `scc_addr` | 0x50F0C020 | Z85C30 |
| `sound_addr` | 0x50014000 | Batman (ASC-compatible) |
| `sdma_addr` | −1 | no 5380 pseudo-DMA |
| `via2_addr` | 0x50002000 (default) | VIA2 (real 6522, not RBV) |
| `c94index[0]` | 2 → `c94info[2]` = {addr 0x50F10000, dreq 0x50F03A00, dreqmask 1, dma 0, slot 0, clk 25} | 53C96 |
| `obv_monitor` | result of `_DAFBMonitorDetect` | built-in DAFB video present and monitor attached |
| `rbv_base_addr`, `rbv_slot` | from ProductInfo video info | DAFB frame-buffer base and pseudo-slot |

Then `cpuinit` selects the 040 MMU helper routines; `setup_040` identity-maps RAM (§2.5).

### 1.3 Spec for the new kernel

- Get the machine ID from the new booter (Gestalt `'mach'`, as NetBSD's Booter passes `MACHINEID`) and keep a table like `boardinit`'s, keyed by Gestalt ID. Quadra 800 = Gestalt 35.
- The ROM version (ROM+8 = $067C) is needed only if the kernel calls the ROM or serves ROM data (ProductInfo) to the Mac environment. The hardware layer doesn't need it.
- Probing is not necessary on the Q800. Probing that is safe: bus-error-protected reads, as A/UX's `pdsprobe`/`probeit` (0x542aa) do.

## 2. Physical memory map (Quadra 800)

### 2.1 Map

| Physical range | Contents | Evidence |
|---|---|---|
| 0x00000000 – top of RAM | RAM, contiguous (§2.2) | [AUX `memcdjsize`] |
| 0x40000000 – 0x4FFFFFFF | ROM (1 MB on the Q800, image seen at 0x40800000; the ROM header's +0x40 long gives the size for ≥ $067C) | [AUX `setupProductInfo`, `getROMSize`] [PUB] |
| 0x50000000 – 0x5FFFFFFF | I/O. Devices are in a 256 KB block at 0x50F00000 that repeats through 0x50000000–0x50FFFFFF *(repeat period: verify)*. A/UX uses both aliases (VIA1 at 0x50000000, SCC at 0x50F0C020). | [AUX] [NBSD `IOBase = 0x50f00000`] |
| 0x60000000 – 0xEFFFFFFF | NuBus super-slot space ($6–$E) | [PUB] |
| 0xF0000000 – 0xFFFFFFFF | NuBus standard slot space, slot *s* at 0xFs000000. Built-in video uses slot $9 space (DAFB); the PDS is slot $E. | [HDR `obvideo.h`] [PUB] |

### 2.2 I/O devices

Offsets are from 0x50F00000 (the 0x50000000 alias works the same way).

| Device | Address | Register layout | Source |
|---|---|---|---|
| VIA1 (6522) | +0x0000 | reg *n* at *n*×0x200 (ORB 0, ORA 0x200, DDRB 0x400, DDRA 0x600, T1C 0x800/0xA00, T1L 0xC00/0xE00, T2C 0x1000/0x1200, SR 0x1400, ACR 0x1600, PCR 0x1800, IFR 0x1A00, IER 0x1C00, ORA-nh 0x1E00) | [HDR `via6522.h`] |
| VIA2 (6522) | +0x2000 | same | [AUX `via2_addr`] |
| MAC address PROM | +0x8000 | | [NBSD if_sn_obio] |
| SONIC DP83932 | +0xA000 | 32-bit registers, 4-byte stride, value in the low 16 bits (offset *i*×4+2) | [NBSD if_sn_obio] |
| SCC Z85C30 | +0xC020 | ch B ctl +0, ch A ctl +2, ch B data +4, ch A data +6 | [AUX `sc_addr_init`, `scputchar`] |
| djMEMC | +0xE000 | +0x2C: top of RAM in 4 MB units (low byte) | [AUX `memcdjsize` 0x582fc] |
| 53C96 DREQ status | +0x3A00 (long) | bit 0 = DREQ | [AUX `c94info[2]`] |
| 53C96 SCSI | +0x10000 | reg *n* at *n*×0x10; pseudo-DMA data port (16-bit) at +0x100 | [AUX c94*.c] [NBSD `SCSIBase = base+0x10000` for Q800-class] |
| Sound (Batman) | +0x14000 | ASC-compatible, plus Batman registers at +0xF09/+0xF29 | [AUX sm.asc.c] |
| SWIM | +0x1E000 | IWM/ISM, reg *n* at *n*×0x200 *(verify spacing)* | [AUX `iwm_addr`] |
| DAFB registers | 0xF9800000 | +0 VBA high, +4 VBA low, +8 row words, +0xC clock cfg, +0x10 DAFB cfg, +0x14 blank enable, +0x18 PGM enable, **+0x1C sense lines**, +0x20 reset, +0x2C read used as a delay | [HDR `obvideo.h`] [AUX `DAFBReadSenseLines`] |
| DAFB CLUT/DAC | 0xF9800200 *(verify)* | | Linux `macfb` DAFB_BASE (GPL, cited as fact only) |
| Frame buffer (VRAM) | 0xF9000000 (+ mode-dependent offset; 0xF9001000 seen under Linux) | | [HDR `OBV_VIDEO_BASE`] |

### 2.3 RAM

- `memsize` (0x57df4) dispatches on `onboard.memctrl`: 1 → `orwellsize` (Q700/900/950), 2 → `memcdjsize` (Q800 class), otherwise `banksize` probing (Mac II/RBV).
- `memcdjsize` reads the long at 0x50F0E02C and takes `(value & 0xFF) << 22` as the end of **one bank starting at 0**. It then zeroes the bank page by page. **The djMEMC presents RAM contiguously from 0**, so the AMIX single-region assumption holds on the Q800.
- NetBSD has an option (`DJMEMCMAX`) that uses the booter-reported memory size on the same five models instead of its own probing *(the reason isn't stated; possibly MacOS under-reports large configurations)*. The new booter should pass the size, and the kernel should cross-check it against djMEMC +0x2C.
- Page 0 holds the Mac low-memory globals and vectors while MacOS runs. A/UX maps page 0 **cache-inhibited, serialized** on the 040 (`setup_040` clears PTE CM and sets CM = 10 for VA 0) because the kernel keeps its own Mac low-memory globals there (§5).

### 2.4 Cacheability

| Region | A/UX 040 setting | Requirement |
|---|---|---|
| RAM | copyback (`cm040` = 0x20 → PTE CM = 01), CACR 0x80008000 | Any mode is fine for CPU-driven I/O. Only the SONIC bus-masters (§4.7). The AMIX port runs write-through; keep that. |
| 0x40000000–0x7FFFFFFF (ROM, I/O) | ITT0 = DTT0 = 0x403FA040: supervisor-only, cache-inhibited **serialized** | I/O must be CI serialized. |
| 0x80000000–0xFFFFFFFF (NuBus, DAFB) | ITT1 = DTT1 = 0x807FA040: same | Registers CI serialized. VRAM may be CI non-serialized or write-through (faster console) *(check that DAFB tolerates burst/line writes before using write-through)*. |

### 2.5 Consequences for the AMIX/040 port

- The port's DTT1 (0x807fa060, CI **non-serialized**, supervisor) covers NuBus and DAFB. Non-serialized is acceptable for VRAM. For DAFB and NuBus-card registers, prefer page mappings with CM = 10 (serialized), or change DTT1 to CM = 10 (0x807fa040) as A/UX does *(cost: slower VRAM writes)*.
- **The I/O block 0x50F00000 is in quadrant 1**, which no TT register covers on the port; the AMIX kernel window is at 0x40000000–0x49FFFFFF. Map 0x50F00000–0x50F3FFFF (and the 0x50000000 alias if drivers use it) in the kernel SRP tree, identity, CI serialized, supervisor. Entry: `plat_iomap[]` in [amix-platform-interface.md §8.1](amix-platform-interface.md).
- The ROM at 0x40800000 conflicts with the kernel window (0x40000000–0x49FFFFFF). It's needed only if the kernel calls the ROM or maps it for `UI_ROM`. Map it at another kernel VA, or map the ROM object for users through `segdev`, as [aux-kernel-design.md](aux-kernel-design.md) already plans.
- RAM is one region from 0. The kernel loads at a PA of the booter's choice (< 0x40000000). Page 0 stays reserved while any ROM code might run.

## 3. Interrupts

### 3.1 CPU levels on the Quadra 800 ("MacOS" mapping, which A/UX 3.1 uses)

| IPL | Source | A/UX handler (vector table `ivect` 0x100120cc, copied to VA 0 by `setup_autovectors`) |
|---|---|---|
| 1 | VIA1 (60 Hz, 1 Hz RTC, ADB shift register, …) | vector 25 → `via10%`: raise to IPL 4 → `Xfdbint` (0x10049160) ADB fast path, then `via1intr` at IPL 1. Egret machines use `egret_via10` → `via1intr` directly. |
| 2 | VIA2 (NuBus/PDS slots + built-in video VBL on port A, SCSI on CB2, sound on CB1) | vector 26 → `via21%` → `via2intr` |
| 3 | on-board Ethernet *only in A/UX interrupt mode* (below); unused in MacOS mode | vector 27 → `faulthdlr` (unused by A/UX 3.1) |
| 4 | SCC | vector 28 → `sc0%` → `sccirq` (`scbypassint`, then `scintr` after `scinit`) |
| 5 | — | `faulthdlr` |
| 6 | — on the Q800 (power/soft-off sources on other models) | vector 30 → `pw0%` → `powerintr` |
| 7 | NMI (interrupt switch) | vector 31 → `AutoVecInt7`: parity checks for IIci/IIfx/Q900/950, otherwise the debugger (`nmiloc`) |

Autovectors only. Spurious (vector 24) → `spurintr`. The common entry `call%` (0x54b5e) saves all registers, switches CACR (`m20cache`), calls the handler with a frame pointer, and on return to user runs soft interrupts, STREAMS and preemption. The IIfx (ID 11) instead uses `AutoVecInt1–6` through the OSS interrupt controller (§6).

**A/UX interrupt mode.** Clearing VIA1 port B bit 6 (`DB1O_AuxIntEnb`, 0 = enabled) and making it an output switches Quadras to a second IPL map: 1 soft, 2 VIA2, 3 Ethernet, 4 SCC, 5 sound, **6 VIA1**, 7 NMI [NBSD machdep.c, intr.c]. NetBSD uses this mode on CLASSQ machines. **A/UX 3.1 does not**: `via1init` never touches VIA1 PB6, and its vector table puts VIA1 at level 1. In MacOS mode the SONIC interrupt arrives as a VIA2 slot-$9 interrupt (port A bit 0) [NBSD if_sn_obio: "otherwise slot 9 via add_nubus_intr()"].

### 3.2 VIA interrupt acknowledge and dispatch (A/UX)

- `via1intr`/`via2intr` (0x100125d4/0x1001264e) loop: `pending = IFR(+0x1A03) | soft_bits; pending &= IER(+0x1C13); pending &= 0x7F`, take the lowest bit (`sindex` table), and call `lvl1funcs[bit]`/`lvl2funcs[bit]` (0x11004da8 / 0x11004dc4). The handler acknowledges by writing `0x80 | 1<<bit` (`smask`) to IFR (`viaclrius`). The +3/+0x13 offsets make the same code work on RBV machines, whose IFR/IER are at 0x03/0x13 [HDR `via6522.h` comment].
- VIA1 bit use (A/UX): bit 0 CA2 → `onesec` (1 Hz from the RTC chip); bit 1 CA1 → `clock` (60 Hz tick); bit 2 SR → `fdb_intr` (ADB); bits 3–6 (CB2, CB1, T2, T1) unused (`noviaint`).
- VIA2 bit use: bit 1 CA1 → `slotintr`; bit 3 CB2 → `scsiirq`, replaced by `c94_intr` on 53C9x machines; bit 4 CB1 → `ASCIntr` (installed by `ASCInit`); bits 0, 2, 5, 6 unused.
- Slot dispatch (`slotintr` 0x100126c8): `pending = ~VIA2 ORA(+0x1E02) & slot_mask` (active low). Bits 0–5 = slots $9–$E → `slotfuncs[]`; bit 6 = built-in video VBL (registered by `video_init` through `viamkslotintr(0, video_intr)`). The handler acknowledges CA1 with `IFR = 0x82` and loops until no slot bit is pending. The slot IRQ lines are level signals owned by the cards; each card handler must clear its own source.
- Enables: `via1init` writes PCR = 0x20, IER = 0x7F (all off), then 0x81 (CA2) on non-Egret machines and 0x82 (CA1) except on the IIfx. `fdb_init` adds 0x84 (SR). `via2init` (Q800 path: not RBV, nc94 = 1) writes PCR = 0x22, IER = 0x88 (CB2 = SCSI), ACR = 0xC0, T1 = 0x1980, then IER = 0x82 (CA1 = slots). `ASCInit` enables 0x90 (CB1) when sound starts.

### 3.3 Fit with AMIX's splhi = IPL 4

- A/UX uses splhi = IPL 7, splclock = 6, spl5 … spl1 as named (0x54884…). The AMIX objects inline IPL 4 for every "high" level.
- On the Quadra 800 in MacOS mode, **every device interrupt is at IPL ≤ 4** (VIA1 1, VIA2 2 including SCSI, sound, slots, SONIC and video, SCC 4). IPL 4 masks them all, so the AMIX policy fits without patching. Only NMI is above it.
- **Don't enable A/UX interrupt mode**: it would put VIA1 (clock, ADB) at IPL 6 and sound at IPL 5, both above splhi.
- Clock at IPL 1 is correct for AMIX (callouts run at tick level; `io_poll[]` every tick).
- Latency-sensitive sources masked by IPL-4 sections:
  - **ADB (VIA1 SR)**: A/UX raises IPL to 4 in the first instruction of the level-1 handler and services the shift register in assembly before lowering to 1 — the ADB transceiver needs the next state change promptly. Under AMIX, long splhi sections can delay this. Handle ADB in a fast level-1 path (as A/UX does) and expect occasional timeouts, which ADB tolerates with a retry.
  - **SCC at 4** (3-byte receive FIFO): overruns at high baud rates are possible during long IPL-4 sections. A/UX has the same exposure (its splhi = 7). Use 9600–38400 for the console.
- The SCSI 53C96 is polled/pseudo-DMA driven; interrupt latency affects only throughput.

## 4. Device specifications

### 4.1 Clock tick (HZ = 60)

**A/UX.** The tick is VIA1 CA1 (`lvl1funcs[1]` = `clock` 0x10014636, which calls `viaclrius` first). On Quadras (non-RBV, nc94 set) `via2init` programs **VIA2 T1 free-running with PB7 square-wave output**: ACR = 0xC0, T1CL = 0x80, T1CH = 0x19, so latch = 0x1980 = 6528. With the 783.36 kHz VIA clock (C15M/20 [PUB]) that is 2 × 6528 counts per period ≈ 60.0 Hz. *(uncertain: this implies that VIA2 PB7 drives VIA1 CA1 on the Quadra; the Mac II-family "60.15 Hz VBL" on VIA1 CA1 comes from the glue logic on older models. Check against the Quadra 800 Developer Note or by measurement.)* A/UX keeps `tick` = 1e6/`v_hz` µs and corrects wall time against the RTC every 120 s (`time_fix_timeout`).

**Spec for the new kernel.** Use **VIA1 Timer 1**, continuous mode, interrupt only:
- ACR = (ACR & 0x3F) | 0x40
- latch = 783360/60 − 2 = 13054 (0x32FE)
- IER = 0xC0
- acknowledge by writing IFR = 0x40, or by reading T1C-L

This doesn't depend on the CA1 wiring, keeps IPL 1, and is how NetBSD drives `hardclock` (VIA1 T1). `hw_clkstart` starts it; `clkreld` writes IER = 0x40. Leave VIA2 T1/PB7 as A/UX programs it, or as the ROM left it *(verify that nothing else depends on it)*. Disable VIA1 CA1 unless it is used as a second time base.

### 4.2 Console and serial: SCC Z85C30

- Registers: base 0x50F0C020; control B/A at +0/+2, data B/A at +4/+6. Write the register number to control, then the value; reading control returns RR0 (or RRn after pointing to it) [AUX `sc_addr_init`, PUB Z85C30].
- **Polled console** (`scputchar` 0x1000856c, `scgetchar`): uses control +2 (channel A, the modem port *(verify port naming)*). If not yet initialised it writes `scitable` (0x11003baa, 15 register/value pairs):
  WR9 = 0x82/0x42 (reset A/B, NV) · WR1 = 0 · WR15 = 0x80 · WR4 = 0x4C (×16, 2 stop bits, no parity) · WR11 = 0x50 (RxC/TxC = BRG) · WR10 = 0 · WR12/13 = 0x0A/0x00 (time constant 10 → 9600 baud with the ≈3.6864 MHz RTxC clock; NetBSD zs uses 9600×384) · WR14 = 0x01 (BRG on, source RTxC) · WR3 = 0xC1 · WR5 = 0x68 · WR1 = 0x13 · WR2 = 0 · WR0 = 0x20 · WR9 = 0x0C.
  Output: at IPL 7, set WR5 |= 0xEA (DTR, RTS, Tx enable, 8 bits), wait for RR0 bit 2 (Tx empty, bounded loop), write data; LF → CR LF. Input: wait for RR0 bit 0, read data, then WR0 = 0x30/0x20/0x10/0x38 (error reset, next Rx int, reset ext, reset IUS).
- **Interrupt driver** (`scinit` 0x10008810): writes `scINITtable` (0x11003c40, 0x1C bytes, WR9 patched to 0x88/0x48 and WR5 base 0x62/0x60 per channel). If PRAM byte 0x89 bit 0 is clear and the machine has IOPs, it uses the IOP (`siopinit`); on the Q800 it takes "Onboard SCC serial driver", `sccirq = scintr`. The vector is autovector 4. `scintr` reads RR3 on channel A to find the pending source and dispatches Rx/Tx/external status (`scrintr`/`scxintr`/`scsintr`).
- `scbypassint` (the initial `sccirq`) quiets both channels (WR0 = 0x10, 0x28, 0x30, 0x38; WR1 = 0) so stray SCC interrupts can't loop before the driver is up. The new kernel should do the same in `config`.
- For AMIX: `putchar` = the polled routine above (any IPL). The console stream driver is a new STREAMS tty driver for the SCC. AppleTalk (LocalTalk) on the SCC (`scc_ltalk.c`) is out of scope.

### 4.3 SCSI: NCR 53C96 (first boot disk)

**Registers** (base 0x50F10000, stride 0x10) [PUB 53C96; AUX c94*.c]: 0x00/0x10 TC lo/hi · 0x20 FIFO · 0x30 command · 0x40 status (read) / dest-ID (write) · 0x50 interrupt (read) / select timeout (write) · 0x60 sequence step · 0x70 FIFO flags · 0x80 CONF1 · 0x90 clock factor · 0xB0 CONF2 · 0xC0 CONF3.

**Init** (`c94_init` 0x10051342, `c94_initchip` 0x10051534):
1. Command 0x02 (reset chip), then 0x00 (NOP).
2. From the clock table at 0x1100dad8 (MHz → CCF, select timeout): 33 → 7/0x9A, **25 → 5/0xA4**, 16 → 3/0x83. The Q800 entry says 25 MHz, so clock factor (0x90) = 5 and select timeout (0x50) = 0xA4 *(the 25 MHz value is from A/UX's table; NetBSD's `esp` for Q800 should be checked for the chip clock)*.
3. CONF1 = 0x47 (bus ID 7, SCSI-reset interrupt disabled), CONF2 = 0, CONF3 = 0x04.
4. Command 0x03 (reset SCSI bus), then 0x00.
5. Wait `c94rdelay` loops (settle time), start a 1 s watchdog timeout.

**Interrupt.** The 53C96 IRQ is VIA2 CB2 (IFR bit 3) at IPL 2. `via2init` enables IER 0x88 and replaces the VIA2 bit-3 handler with `c94_intr`. `c94_intr` acknowledges the VIA (`viaclrius`), then for each controller with status bit 7 set it reads status (0x40), sequence step (0x60 & 0xF) and interrupt (0x50; the read clears the chip interrupt), then runs the controller's state function. It loops until no chip reports an interrupt.

**Selection** (`c94_select`): command 0x01 (flush FIFO); write the identify message (0xC0 when disconnect is allowed) and the CDB bytes to the FIFO; write the target ID to 0x40; command 0x41 (select without ATN) or 0x42 (select with ATN). Status phase: command 0x11 (initiator command complete).

**Data transfer — pseudo-DMA, no bus-master DMA on the Q800** (`c94info.dma` = 0):
- `c94_blindin`/`c94_blindout` (256-byte chunks):
  1. TC = 0x0100; command 0x90 (DMA | transfer info).
  2. Poll the DREQ long at **0x50F03A00, bit 0** (Q700/900: 0xF9800024/0xF9800028, bit 0x200, [HDR `DREQ0/1`, `ST_DREQ`]) for the first two words.
  3. Move the rest of the 128 words with back-to-back 16-bit reads or writes of **regbase + 0x100**.
  
  The hardware stalls the bus cycle until data is ready. The loop runs under `u.u_nofault`/`setjmp`, so a bus error (handshake timeout, e.g. phase change) aborts the chunk.
- Remainders and odd bytes: `c94_pollin`/`c94_pollout` (programmed I/O through the FIFO, command 0x10 transfer info, per-byte interrupt polling with `c94_waitirq`).
- `c94_hdmain` (true DMA through a controller at `c94info.dma`, command 2, physical address) is only for the Quadra 900/950 PDS SCSI card (board ID 0x48D in slot $E).
- Maximum per request: 0x8000 bytes (`c94_vio`).

**Spec for the new kernel.** A 53C9x driver with this init. Pseudo-DMA chunks of 256 bytes (or a whole block) through +0x100 while guarded against bus errors; PIO fallback. Interrupt via VIA2 CB2. The NetBSD `esp` driver for mac68k (obio) is the BSD reference *(verify its DREQ address for the Q800 against A/UX's 0x50F03A00)*. The boot disk path needs: reset, INQUIRY, READ CAPACITY, READ(10), WRITE(10). A/UX's partition code (Apple Partition Map, `gdisk.c`) is a separate spec.

### 4.4 ADB keyboard and mouse (VIA-based transceiver; no Egret/Cuda on the Q800)

- **Hardware** [NBSD adb_direct.c `ADB_HW_II` for Q800/Q700/Q650/Q610/C650/C610/IIci; PUB Guide ch. "ADB"]: VIA1 PB4/PB5 = ADB state outputs ST0/ST1 (00 = new command, 01/10 = even/odd data byte, 11 = idle); VIA1 PB3 = ADB interrupt/SRQ input (active low); the VIA1 shift register carries the bytes, clocked by the transceiver on CB1; SR interrupt = IFR bit 2.
- **A/UX** (`fdb.c`; `fdb_init` 0x10041f20, `via_fdb_start` 0x10042938, `Xfdbint`/`fdb_inthand` 0x10049160/0x100491c6):
  - Init: ORB |= 0x30, DDRB |= 0x30 (state = idle), IER = 0x84.
  - Start a transaction at IPL 4: ACR = (ACR & 0xE3) | 0x1C (shift out under external clock); ORB &= 0xCF (state 0); write the command byte to SR. Command byte: talk = `addr<<4 | 0x0C | reg`, listen = `addr<<4 | 0x08 | reg`, flush = `addr<<4 | 0x01`, reset = 0x00 (`fdb_current` 0x10) *(0x0F appears for command code 4; SendReset is normally 0x00)*.
  - The per-byte state machine is in assembly at IPL 4. It acknowledges with IFR = 0x04, reads or writes SR, toggles PB4/PB5 through the even/odd states (`SetS2`/`SetS3`), and sets `via1_soft` bit 2 at the end. `fdb_intr` at IPL 1 then completes the request, polls devices with pending SRQ, and starts the next queued command.
- **Spec**: port NetBSD's ADB_HW_II state machine as the reference; A/UX's fast level-1 path shows the latency requirement. Keyboard: device 2, talk register 0 (two key codes, bit 7 = up). Mouse: device 3, talk register 0 (button bit 7 of byte 0, 7-bit signed deltas). Autopoll: repeated talk R0 to the last active device. The Mac key-code map is Apple public data (Inside Macintosh); the A/UX `key.c`/`keyboard.c` layer maps it to the console.
- Soft power-off on this class: **VIA2 PB2** (`VRB_POWEROFF`/`v2PowerOff`): `dopowerdown` clears ORB bit 2 and sets DDRB bit 2 [AUX 0x59088, HDR `via6522.h`, NBSD `DB2O_v2PowerOff`].

### 4.5 RTC and PRAM

- **A/UX calls the ROM**: `ReadXPRam` (A051), `WriteXPRam` (A052), `ReadDateTime` (A039), `SetDateTime` (A03A) (0x10049d46…0x10049dcc). Each call runs at IPL 7 with the kernel's A-line vector (`kernelAline`) temporarily in $28, dispatching into the ROM trap dispatcher that `initMacEnvironment` set up (§5). PRAM uses: time-zone/DST (XPRAM 0xEC, 4 bytes), the 'NuMc' validity check (0x0C), the default video slot (0x80), serial compatibility (0x89). `/dev/nvram` (`nvram.c`) exposes 256 bytes through these calls.
- **Hardware** (Q800 = II-style, not Egret) [NBSD pramasm.s, viareg.h; PUB Guide ch. "RTC"]: VIA1 PB0 = data (bidirectional through DDRB bit 0), PB1 = clock, PB2 = /enable (active low).
  - Serial, MSB first, 8-bit commands.
  - Seconds: read `0x81 | (n<<2)`, write `0x01 | (n<<2)`, n = 0..3 (byte 0 = LSB of the 32-bit seconds since 1904).
  - Write-protect register: 0x35, with value 0x55 (clear) / 0xD5 (set).
  - Old 20-byte PRAM: 0x41-style single-byte commands (0x20–0x3F group and 0x08–0x0B).
  - 256-byte XPRAM: two-byte command `0x3880 | (addr bits rotated in)`: 0x38/0xB8 with the address split over the two bytes.
  - The RTC also produces the 1 Hz VIA1 CA2 interrupt (`onesec`).
- **Spec**: implement the VIA bit-bang natively (no ROM). Take the exact XPRAM address encoding from pramasm.s or the Guide's RTC chapter. Keep A/UX's time model: Unix time = Mac seconds − 0x7C25B080 (1904 → 1970), adjusted by the XPRAM 0xEC GMT offset (24-bit signed seconds, bit 31 = DST) [AUX `Mac2UnixTime`, `init_time`].

### 4.6 Video: built-in DAFB (console frame buffer)

- **A/UX detection**: `_DAFBMonitorDetect` (0x5838c) reads the sense lines. For each of the patterns 3, 5, 6 written to 0xF980001C, it reads the register back, inverts it and masks it to 3 bits (with `DAFBResetDelay` = 4 reads of +0x2C). Monitor ID 7 means "extended sense" (`ReadExtendedSense`); an extended code of 0x3F means no monitor. The result is `obv_monitor`.
- **A/UX initialisation — entirely through the ROM** (`video_init` 0x10047954 → `initMacEnvironment` → Slot Manager):
  1. `video_find` enumerates `availSlots` plus the built-in pseudo-slot. It gets the device base with Slot Manager calls (`_SlotManager` selectors 0x15 sNextTypesRsrc / 0x1B / 0x06 / 0x05) and walks the video mode sResources (0x80 upward). `getVPBlock` chooses the **lowest-depth mode** (normally 1 bpp) and records its VPBlock (base offset, rowBytes, bounds, pixel size).
  2. `getVideoDriver` (selector 0x2D) finds the declaration-ROM driver, which is opened with `callOpen` (`callDriver`). XPRAM 0x80 names the main screen.
  3. The console text is then drawn by the kernel into VRAM (`vidbitmap.c`, `vt100.c`). The VBL interrupt is VIA2 slot bit 6.
- **Spec for the new kernel, no ROM calls:**
  - *Stage 1 (enough for a console)*: MacOS has already initialised DAFB at boot in the depth chosen in the Monitors control panel. The booter passes the frame-buffer physical base, rowBytes, width, height and depth (as NetBSD's Booter does; QuickDraw `GDevice`/`PixMap` of the main screen). The kernel draws 1- or 8-bit text into that buffer. In 8 bpp, keep the CLUT MacOS loaded: in the standard 8-bit Mac CLUT index 0 = white and 0xFF = black; in 1 bpp, bit 1 = black. No DAFB register writes are needed.
  - *Stage 2 (optional)*: program the CLUT through the DAC at 0xF9800200 *(verify register protocol from public DAFB/Antelope descriptions)*. Mode changes would need the DAFB clock/timing registers (`obvideo.h` names) and are out of scope; A/UX never touches them directly.
  - VRAM access: CI (TT1). Cursor/VBL: VIA2 port A bit 6 if needed.
  - The ROM-mapped video path (the Mac environment's own drivers through `UI_PHYS_SCREENS`) is covered in [aux-kernel-design.md](aux-kernel-design.md).

### 4.7 Ethernet: on-board SONIC (DP83932)

- **A/UX 3.1 on this image has no SONIC driver.** `boardinit` records `onboard.ether = OBEtherSonic`, but the only kernel use is in `sysslotmanager` (0x10046df4), which treats the built-in pseudo-slot as present for the Mac side's Slot Manager calls. `ae6` in `/etc/boot.d` is an 8390-type NuBus card driver (`ae6_rpkt`, `get_boundary_page`, …). There is no Ethernet vector (level 3 → `faulthdlr`). The spec therefore comes from the datasheet and NetBSD.
- **Hardware** [NBSD if_sn_obio; PUB DP83932]:
  - Registers at 0x50F0A000, stride 4, data in the low 16 bits (offset *i*×4+2).
  - 32-bit bus mode. DCR = `BMS | RFT1 | TFT0 | EXBUS` (block-mode DMA, FIFO thresholds, extended bus) for the Q800/Q700/Q900/Q950/Q610/Q650/C610/C650; DCR2 = 0.
  - MAC address from the PROM at 0x50F08000. NetBSD has a fallback that reads CAP0–2 after the ROM has run *(the PROM byte order/bit reversal per model: verify; Apple OUIs 08:00:07 / 00:05:02 help detect bit-reversed bytes)*.
- **Interrupt**: in MacOS mode it comes through VIA2 as slot $9 (port A bit 0, IPL 2); in A/UX interrupt mode it would be autovector 3 (not used, §3.1).
- **DMA**: the SONIC bus-masters descriptors and buffers (receive resource area, receive descriptors, transmit descriptors, CAM descriptors) in main RAM with 32-bit addresses. Allocate these areas from physically contiguous memory **mapped cache-inhibited** (NetBSD does). Otherwise push/invalidate lines around each transfer (`cpushl`/`cinvl`) — with the port's write-through data cache only invalidation before reading received data is needed.
- **Init** (datasheet): software reset (CR bit RST), DCR/DCR2, load CAM, RRA/RDA/TDA pointers (URRA/RSA/REA/RRP/RWP, URDA/CRDA, UTDA/CTDA), RCR, IMR, then clear RST and RXEN.

### 4.8 Floppy: SWIM (lower priority)

- Address 0x5001E000 (`iwm_addr`). The chip starts in IWM mode; A/UX switches to ISM ("SWIM") mode for MFM 1.44 MB (`ism_chipsubs.c`) and uses IWM mode for GCR 400/800 KB (`woz_chipsubs.c`, `gcrsubs.c`, `mfmsubs.c`). The drive head select is VIA1 PA5 (`VRA_HEAD`, [HDR]).
- Not interrupt-driven on the Q800 (polled, timing-critical loops at high IPL). On Q900/950/IIfx the floppy goes through the SWIM IOP (`iop_intr_swim`).
- Spec later: IWM/SWIM register map from the Guide's "SWIM" chapter; A/UX `*_chipsubs.c` for command sequences.

### 4.9 Sound: Batman (lower priority)

- Base 0x50014000 (0x50F14000). ASC-compatible: FIFOs at +0x000/+0x400 (A/B, 1 KB each; `chord` writes +0/+0x200/+0x400/+0x600 for the four wavetable voices), version +0x800, mode +0x801, control +0x802, FIFO mode +0x803, volume +0x806, clock/rate +0x807, FIFO IRQ status +0x804 *(verify from ASC documentation)*. Batman adds per-channel interrupt masks at **+0xF09 and +0xF29** (written 1 to mask, 0 to enable) and +0x80A [AUX `ASCIntr` 0x1007d5a0].
- The interrupt is VIA2 CB1 (IFR bit 4, IPL 2). `ASCInit` sets `lvl2funcs[4] = ASCIntr`, enables with IER = 0x90 / 0x10 and acknowledges with IFR = 0x90. The beep (`chord` 0x10049eac) writes the FIFOs directly at IPL 7.
- Spec later (beep first).

### 4.10 Other on-board items

- **NuBus/PDS slot interrupts**: VIA2 port A bits 0–5 (slots $9–$E), active low, level; `slot_mask` 0x7F [AUX].
- **VIA2 PB1** (`VRB_BUSLOCK`, "NuBus transactions are locked"): `via2init` sets ORB |= 2 on all non-RBV machines [AUX, HDR].
- **Cache/parity**: the Q800 has no parity RAM or parity NMI path (A/UX parity handlers are IIci/IIfx/Q900/950 only).
- **Reboot**: A/UX `doboot` (0x55c8e) jumps to the ROM reset path *(not analysed; the port's `haltsys` needs MMU/caches off and a jump to the ROM's start at 0x4080002A or an RTC/VIA-free reset; verify)*. Power-off: VIA2 PB2 (§4.4).

## 5. A/UX start-up on the Quadra 800, and ROM dependence

Order [AUX `_start` 0x54000, `vadrspace` 0x5634c, `startup` 0x56c5a]:

1. IPL 7. Record `kernel_info`, `machineID`, section info. Zero BSS (physical). VBR = 0x50000 (physical scratch vector page).
2. CPU detection by trapping on 030/040-only instructions: `cputype` 5 = 040. On the 040 it saves the **ROM's FP exception vectors** from Mac low memory $C0, $CC, $D0, $D4, $D8 (BSUN, UNFL, OPERR, OVFL, SNAN) into `mac_interr_vecs`, i.e. the ROM's 040 FP support *(how they are used later: not traced)*. The AMIX port brings its own FPSP.
3. `cpuinit`, `boardinit` (§1.2), `memsize` (§2.3; `memerror%` catches bus errors).
4. `mmusetup` builds the root table. `vadrspace` maps kernel text/data/bss (0x10000000/0x11000000/0x12000000, u-area 0x12FFF000, kernel stack top 0x13000000) and the RAM banks. On the 040 `setup_040` identity-maps all RAM with copyback, makes page 0 CI serialized, and loads **ITT0 = DTT0 = 0x403FA040, ITT1 = DTT1 = 0x807FA040**. `turnon_mmu040`: URP/SRP from `cpu_rp`/`sup_rp`, `pflusha`, `cpusha`, **TC = 0x8000** (4 KB pages), VBR = 0. `romaline` = the Mac A-line vector at boot (kernel image + $28).
5. `setup_lowmem_globals` (UnivInfoPtr $DD8 → the kernel's ProductInfo copy), `setup_autovectors` (copies `ivect` to VA 0), `mktables`, and so on.
6. `init_first[]`: `strinit`, `via1init`, `via2init`, `egret_init`, `iop_init`, **`video_init`** (→ `initMacEnvironment`), `fdb_init`, `key_init`, `mouse_init`, `dispinit`, `sndinit`. `init_second[]`: `ufsinit`. In `startup`, `init_normal[]`: `shlinit`, `shlrinit`, `scinit`, `c80_init`, `c94_init`, `parityinit`, `cacheinit`, `mminit`, …, `BNETinit`, `atp_init`, `llap_init`. Then `init_time` (reads the RTC through the ROM).
7. `main` (0x100077e8).

**ROM calls A/UX relies on** (all from `initMacEnvironment` 0x10046148 and the wrappers at 0x10049bf4–0x10049e92):

| ROM use | Why | Needed by the new kernel? |
|---|---|---|
| `ROMArray[0]` = InitDispatcher (Foreign OS table at ROM+[ROM+$16]); the OS trap table at $400 is filled with kernel patches (`OSPatch`), unimplemented traps → `noSupport` panic | a mini Mac environment in kernel low memory (ROMBase $2AE = 0x40800000, MMU32Bit $CB2 = 1, HWCfgFlags bit 1, UnitTable, …) | **No** |
| StartSDeclMgr + `secondaryInit` (0x408062BC on $067C ROMs, 0x408062C0 on IIci/IIfx), `_SlotManager` (patched by `aSlotManagerPatch`), declaration-ROM video drivers (`callDriver`, `callOpen`) | NuBus/PDS card and built-in video discovery and mode setting | **No** for the Q800 console (booter-supplied frame buffer). Only needed to support arbitrary NuBus video cards without native drivers. |
| `_ReadXPRam`, `_WriteXPRam`, `_ReadDateTime`, `_SetDateTime` | RTC/PRAM | **No**: VIA bit-bang (§4.5) |
| Egret (`cEgretDispatch`) | kernel code, not ROM | n/a on the Q800 |
| ROM FP vectors ($C0–$D8) | 040 FP exceptions | **No** (AMIX FPSP) |
| ProductInfo (UnivInfoPtr) | video base, Mac-side `UI_GET_PRODINFO` | Hardware layer: no. The A/UX-compat layer serves ProductInfo to Mac processes (from the booter or the ROM image). |

**Conclusion.** The new kernel can run the Quadra 800 without calling the ROM, provided the booter passes machine ID, memory size, frame-buffer parameters and (optionally) the ROM physical address. The ROM must stay mappable for Mac processes (`UI_ROM`), but the kernel never executes it. This avoids A/UX's constraints: page-0 low-memory globals, IPL-7 ROM calls, and the A-line vector swapping.

## 6. Other $067C machines (for later)

| | IIci (ID 9) | IIfx (ID 11) | Quadra 700 (ID 20) | Quadra 900/950 (ID 18/24) |
|---|---|---|---|---|
| CPU | 030 25 MHz | 030 40 MHz | 040 25 MHz | 040 25/33 MHz |
| VIA2 | **RBV** at 0x50026000 (IFR 0x03, IER 0x13, slot IFR 0x02, slot IER 0x12, monitor type 0x10) | none: **OSS** interrupt controller at 0x50F1A000 (per-source IPL bytes 0–15; A/UX `setup_oss`: slots 2, video 1, SCC-IOP 4, sound 2, SCSI 2, 60 Hz 1, VIA1 1; status word at +0x202) | VIA2 0x50002000 | VIA2 0x50002000 |
| IRQ entry | as Q800 | `AutoVecInt1–6` → `Level1Int`… decoding OSS | as Q800 | as Q800 (Egret: `egret_via10`) |
| Memory | RBV banks: bank A at 0, bank B at **0x04000000** when bank A is full (`banksize` probing), so **discontiguous**. Built-in video uses main RAM (frame buffer in bank A at 0, `setup_rbv` remaps it) | `banksize`; RPU parity 0x50F1E000 | **Orwell** at 0x50F0E000 (bank config bits; `orwellsize`), parity on Q900/950 | Orwell |
| SCSI | **NCR 5380** at 0x50010000, pseudo-DMA 0x50012000, handshake 0x50006000 (`scsidma.c`) | 5380 at 0x50F08000 with hardware handshake | 53C96 at 0x50F0F000, DREQ 0xF9800024 bit 0x200, 25 MHz | as Q700 + external bus 0x50F0F402 / DREQ 0xF9800028; PDS DMA card 0xFE000000 |
| SCC | 0x50004000 | via **IOP** 0x50F04000 (bypass possible; SCC 0x50F04020) | 0x50F0C020 | via IOP 0x50F0C000 (SCC 0x50F0C020) |
| ADB / RTC | VIA (ADB_HW_II) | via IOP / VIA RTC *(verify)* | VIA | ADB via IOP (NBSD) or Egret (A/UX sets `egret`); Egret power/RTC *(verify)* |
| Video | RBV built-in (`rbv_monitor`, VIA2 +0x10 monitor bits) | NuBus cards only | **DAFB** 0xF9800000 (same as Q800) | DAFB |
| Floppy | SWIM 0x50016000 | SWIM IOP 0x50F12000 | SWIM 0x5001E000 | SWIM IOP 0x50F1E000 |
| Ethernet | — | — | SONIC | SONIC |
| Tick | VIA1 CA1 (RBV 60.15 Hz) | OSS 60 Hz source | as Q800 | as Q800 |
| IPL > 4 sources | none | OSS assignable (A/UX uses ≤ 4) | none (MacOS mode) | L6 power (`pw0`/`powerintr`) *(verify)* |

The Quadra 610/650 and Centris 610/650 are identical to the Q800 except for CPU clock (`boardinit` IDs 28/34/50/51) and slot count. The 68030 machines need the 030 MMU layer of the AMIX port and the 5380 SCSI driver; the IIfx needs OSS and IOP support (or IOP bypass mode, as NetBSD uses).

## 7. Open items

- Whether VIA2 T1/PB7 drives VIA1 CA1 on the Quadra (§4.1). It doesn't matter if VIA1 T1 is used.
- The Q800 53C96 clock (25 MHz per A/UX) and the DREQ register at 0x50F03A00, against NetBSD `esp` and the Developer Note.
- SONIC MAC-address PROM byte and bit order on the Q800.
- DAFB CLUT/DAC access protocol (only for stage-2 video).
- Reboot mechanism without the ROM.
- ASC/Batman register names beyond what A/UX touches.
- The I/O alias period of the 0x50F00000 block; the new kernel should use 0x50F0xxxx addresses only.
