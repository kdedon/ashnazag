# Kickstart 3.2 ROMs: static analysis for the guest container

What the Amiga part of [guest-container-design.md](guest-container-design.md) (§6) relies on in the Kickstart 3.2 ROMs.

**Inputs**: the AmigaOS 3.2 CD (`/ROM/*.rom`) and the public NDK 3.2 (`exec/execbase.i`, `exec/resident.i`, `hardware/custom.i`, `graphics/gfxbase.i`, FD files, `expansion.doc`).
**Method**: static only. `tools/amiga/romtag.py` (RomTag scan as exec does it), `tools/amiga/kdis.py` (capstone disassembly naming custom/CIA/Gayle/Gary/autoconfig addresses, tracking `lea $DFF000,An` bases), `tools/amiga/kmods.py` (per-module census of hardware and privileged instructions). Addresses below are ROM addresses of the **A1200 image** unless stated; file offset = address − $F80000. The A500/A600 image has the A1200 layout shifted by −$68 from `diag init` on; A4000T equals A4000 up to `$F89870`.

| Image | SHA-1 (prefix) | Version | Model-specific modules |
|---|---|---|---|
| `kicka1200.rom` | `5b2982876fec2166` | 47.96, exec 47.7 | IDE `scsi.device`, `card.resource`, `carddisk.device` |
| `kickCDTVa1000a500a2000a600.rom` | `b88e364daf23c9c9` | 47.96 | same set as A1200 (A600 Gayle) |
| `kicka3000.rom` | `7bc0e75622d7254e` | 47.96 | `A3000 bonus`, SDMAC `scsi.device` |
| `kicka4000.rom` | `37a8aa0b83782d75` | 47.96 | `A4000 bonus`, IDE `scsi.device` |
| `kicka4000t.rom` | `ede1748eb2cbb1e8` | 47.96 | as A4000 plus `NCR scsi.device` (53C710) |

Static limits: a linear sweep also decodes data, and hardware reached through a base held in a structure (`cia.resource`, `scsi.device`) is only partly named. Addresses cited below were confirmed by reading the code there; the per-module register lists come from the census.

## 1. Resident modules

Flags: S = `RTF_SINGLETASK`, C = `RTF_COLDSTART`, A = `RTF_AFTERDOS`, auto = `RTF_AUTOINIT`. Offsets per image (`romtag.py` prints init entries too).

| Module | Ver | Pri | Flags | A1200 | A500/600 | A3000 | A4000 | A4000T |
|---|---|---|---|---|---|---|---|---|
| expansion.library | 47 | 110 | S | $04174 | $04174 | $04118 | $0417C | $0417C |
| exec.library | 47 | 105 | S | $00042 | $00042 | $00042 | $00042 | $00042 |
| diag init | 47 | 105 | C | $0418E | $0418E | $04132 | $04196 | $04196 |
| utility.library | 47 | 103 | C+auto | $41320 | $412B8 | $560C8 | $56E8C | $5B160 |
| A3000 / A4000 bonus | 40 | 101 | C | — | — | $04CF8 | $04CE8 | $04CE8 |
| potgo.resource | 37 | 100 | C+auto | $446FC | $44694 | $42368 | $4312C | $47400 |
| cia.resource | 45 | 80 | C | $44118 | $440B0 | $0E450 | $0F214 | $134E8 |
| FileSystem.resource | 47 | 80 | C | $448D4 | $447C4 | $1E998 | $1F75C | $23A30 |
| battclock.resource | 47 | 70 | C | $421B4 | $4214C | $099F4 | $0A7B8 | $0EA8C |
| misc.resource | 37 | 70 | C | $4482C | $449F0 | $422C0 | $43084 | $47358 |
| disk.resource | 47 | 70 | C | $44B00 | $44A98 | $15140 | $15F04 | $1A1D8 |
| battmem.resource | 39 | 69 | C | $44514 | $444AC | $0A488 | $0B24C | $0F520 |
| graphics.library | 47 | 65 | C | $058C4 | $0585C | $262C0 | $27084 | $2B358 |
| layers.library | 46 | 64 | C+auto | $32184 | $3211C | $3EDE4 | $3FBA8 | $43E7C |
| gameport.device | 47 | 60 | C+auto | $49014 | $48FAC | $46CD0 | $47A94 | $4BD68 |
| timer.device | 46 | 50 | C | $45F44 | $45EDC | $53674 | $54438 | $5870C |
| card.resource | 47 | 48 | C | $4073C | $406D4 | — | — | — |
| keyboard.device | 47 | 45 | C+auto | $48890 | $48828 | $4654C | $47310 | $4B5E4 |
| keymap.library | 47 | 40 | C+auto | $452B8 | $45250 | $3E154 | $3EF18 | $431EC |
| input.device | 47 | 40 | C+auto | $47DA0 | $47D38 | $45A5C | $46820 | $4AAF4 |
| ramdrive.device | 46 | 25 | C+auto | $435F4 | $4358C | $44FD0 | $45D94 | $4A068 |
| trackdisk.device | 47 | 20 | C | $49A3C | $499D4 | $544A4 | $55268 | $5953C |
| carddisk.device | 47 | 15 | C | $42C48 | $42BE0 | — | — | — |
| scsi.device | 47 | 10 | C | $351CC | $35164 | $05A6C | $05A5C | $05A5C |
| NCR scsi.device | 47 | 10 | C | — | — | — | — | $09870 |
| intuition.library | 47 | 10 | C+auto | $590EC | $59084 | $56F5C | $57D20 | $5BFF4 |
| syscheck | 47 | −35 | C | $3F760 | $3F6F8 | $0E3E8 | $0F1AC | $13480 |
| romboot | 45 | −40 | C | $46D74 | $46D0C | $52648 | $5340C | $576E0 |
| bootmenu | 47 | −50 | C | $3B9E8 | $3B980 | $0A670 | $0B434 | $0F708 |
| alert.hook | 47 | −55 | C | $040C0 | $040C0 | $04064 | $040C8 | $040C8 |
| strap | 45 | −60 | C | $46D8E | $46D26 | $52662 | $53426 | $576FA |
| filesystem | 47 | −81 | – | $26C60 | $26BF8 | $1EC00 | $1F9C4 | $23C98 |
| ramlib | 45 | −100 | A | $44E8C | $44E24 | $45630 | $463F4 | $4A6C8 |
| audio.device | 47 | −120 | auto | $3F7C8 | $3F760 | $08A7C | $09840 | $0DB14 |
| dos.library | 47 | −120 | – | $1D78C | $1D724 | $15500 | $162C4 | $1A598 |

Not listed (no hardware, all images, pri −120…−123): mathieeesingbas (C), mathffp, console, gadtools (auto), workbench, icon, workbench.task, con-handler, syslog, shell, system-startup, ram-handler.

**Hardware touched** (confirmed sites; all modules also write `INTENA` for inline `Disable`/`Enable`: 160 `move.w #$4000/$C000,$DFF09A.l` sites in the A1200 image):

| Module | Registers | Design §6.1 | Change |
|---|---|---|---|
| exec | see §2 | keep | also CPU/FPU probe via `movec`, `$DE1000` Gayle ID (execPrivate18, LVO −816, `$F80504`) |
| expansion | `$E80000` autoconfig; RAM probe from `$08000000` up (A1200/A3000/A4000), from `$07F00000` down (A3000/A4000); Gary `$DE0000`←$80 (A3000/A4000) | keep | fast RAM at `$08000000` is found by the ROM itself (§3.4) |
| diag init | autoconfig DiagAreas of configured boards | keep | none |
| A3000/A4000 bonus | Ramsey `$DE0043` (revision), `$DE0003` (control, written then polled until read-back matches) | not listed | keep; `$DE0043` reads $7F → skipped |
| cia.resource | CIA-A/B `icr`, `pra`, `ddra`; level 2/6 servers | keep | none |
| potgo.resource | `POTGO` | keep | none |
| battclock / battmem | RTC `$DC0000` | replace | none |
| disk.resource | CIA-B `prb`, CIA-A `pra`/`ddra`, `DSKLEN`, `DMACON` | stub | none |
| graphics | §6 | keep | adds CIA-A TA/TOD, CIA-B TOD, `BPLCON0` ERSY, `BEAMCON0`, `FMODE` |
| timer.device | §5 | replace | none |
| card.resource / carddisk | Gayle `$DA8000–$DAB000`, `$A00000` attribute memory | replace | card.resource adds nothing unless Gayle ID high nibble = $D (`$FC0838`): no stub needed |
| keyboard.device | CIA-A `cra` (SP mode), `sdr` | replace | none |
| gameport.device | CIA-A `pra`/`ddra` (fire), `JOYxDAT`/`POTGO` through a base register | replace | none |
| trackdisk.device | CIA-A `pra`, CIA-B `prb`, `DSKPT`, `DSKLEN`, `ADKCON` | replace | none |
| scsi.device | A1200/A600: Gayle IDE `$DA2000`; A3000: SDMAC `$DD0000`; A4000: IDE `$DD2020`; `tst.b $BFE001` delays (55 sites) | replace | A4000T also needs `NCR scsi.device` (`$DD0040`) outranked |
| audio.device | `AUDx`, `DMACON`, `ADKCON`, `INTENA`/`INTREQ` | replace | none |
| intuition | `DMACON`, `POTINP` | keep | none |
| syscheck → bootmenu | CIA-A `pra` (left button), `POTINP` (right button) | keep | mouse buttons held at reset open the Early Startup menu |
| strap | `DMACON`, `INTENA` (boot animation) | keep | none |

## 2. Early init: reset → `InitCode(RTF_COLDSTART)`

Reset PC `$F800D2` → `bra $F80152`. Order of hardware accesses:

| # | Address | Access | Value expected / behaviour | Images |
|---|---|---|---|---|
| 1 | `$F80152` | SP = `$400`; sum all longs of `$F80000–$FFFFFF` with end-around carry | must be $FFFFFFFF (fix-up long at file `$7FFE8`) | all |
| 2 | `$F80178` | read word `$F00000` | = $1111 → `jmp $F00002`, return address in A5 | all |
| 3 | `$F8018C` | `$DA8000` ← 0; read bytes `$A00000/2/4` | = $91,$05,$23 → boot from PCMCIA (`jmp $600000+n`); then `$DA8000` ← 1 | A1200, A500/600 |
| 4 | `$F801DC` | `$BFA001` ← 0, `$BFA201` ← 0 (CIA-A `pra`/`ddra` aliases) | absorbed | A1200, A500/600 |
| 5 | `$F801E8` | CIA-A `pra` ← 0 (overlay off), `ddra` ← 3 | absorbed | all |
| 6 | `$F801F6` | `INTENA`, `INTREQ`, `DMACON` ← $7FFF; `SERPER` ← $174; `BPLCON0` ← $200; `BPL1DAT` ← 0; `COLOR00` ← $111 | absorbed | all |
| 7 | A4000 `$F801CA` | Gary `$DE0000`, `$DE0001` ← 0; `movec #9,CACR`; if CACR ≠ 0 and `$DE0002` bit 7 set: clear `$0`, `$4` (power-on ⇒ cold start; A3000 also runs `fmove`); `bclr #7,$DE0002` | `$DE0002` bit 7 must read 0 or the prepared ExecBase is discarded | A3000, A4000(T) |
| 8 | `$F8022E` | write/read back vectors `$8–$BC` | RAM | all |
| 9 | `$F80262` | old ExecBase check (§3.1); ColdCapture | — | all |
| 10 | `$F80CF8` | CPU/FPU probe (§8) | host-CPU CACR semantics | all |
| 11 | `$F802BA` | chip RAM: pattern $F2D4B689 at each 16 KB up to `$200000`, alias check against `$0` | 2 MB, non-destructive | all |
| 12 | `$F80300`, `$F8037C` | long at `$0`: 'LOWM' (chip list from `$400`), 'HELP' (alert from `$100`) | must be neither | all |
| 13 | `$F80438` | slow RAM `$C00000–$D9FFFF` in 256 KB steps; mirror test writes `INTENA` and reads `$C0F09A`, `$C4F01C` | reads must not fault; writes must not stick ⇒ none found | all |
| 14 | `$F803F6` | RomTag scan (§3.2) | — | all |
| 15 | `$F80402` | `InitCode(RTF_SINGLETASK)`: expansion (§1), then exec's own init | — | all |
| 16 | `$F806A8` | vectors `$8–$3FC` ← exec handlers (default `$F81A58`); 020+: `CACR` \|= $808; 040: `cpusha` | — | all |
| 17 | `$F807E4` | `DMACON` ← $8200, `INTENA` ← $C000 | — | all |
| 18 | `$F8087C` | `andi #0,SR`: boot task continues in **user mode** | — | all |
| 19 | `$F80888` | CoolCapture, then `InitCode(RTF_COLDSTART)` (`$F80896`) | — | all |

Register answers the replacement layer must give (boot phase):

| Register | Answer | Why |
|---|---|---|
| `VPOSR` bits 14–8 | A1200/A4000: $22 (Alice PAL) / $32 (NTSC); A3000: $20/$30; A500: $00/$10 or ECS | graphics HR_AGNUS from bit 5, NTSC from bit 4; SetChipRev AA_ALICE from bit 1 |
| `VPOSR`/`VHPOSR` beam | advancing with host time, wrapping at 312/262 lines; frozen while `BPLCON0` bit 1 (ERSY) is set | graphics waits for line 20…160 (`$F8F362`, hangs if frozen); ERSY test (`$F8F378`) reports genlock if the beam moves |
| `DENISEID` | $00F8 Lisa (AGA), $00FC ECS Denise; stable across 17 reads | probe `$F94998`; low nibble 9 or a changing value = OCS |
| `DMACONR` bit 14 (BBUSY) | 0 | `WaitBlit` loops (`$F85F80` and 20 more) |
| `INTENA`/`INTENAR`, `INTREQ`/`INTREQR` | full set/clear semantics; a **software write of `INTREQ` with SET raises the level** (`Cause` $8004 at `$F8195A`, graphics $8040 at `$F880FC`, timer $8008 at `$FC69A4`) | exec, graphics, timer |
| CIA-A TA, TB | count down at the E-clock; one-shot and continuous modes; `icr` flags | graphics TODA test (`$F8F3EE` loops while TA ≥ $8000); ROM timer.device |
| CIA-A TOD | 50/60 Hz from host time | ROM timer.device calibration hangs without it (`$FC620A`); graphics sets TODA_SAFE if it ticks |
| Gayle ID `$DE1000` | constant (0 or $FF) ⇒ "no Gayle" | `$F80504`; ROM IDE scsi.device and card.resource then stay idle |
| `$DE0002` bit 7 | 0 | step 7 |
| Ramsey `$DE0043` | $7F | A3000/A4000 bonus skips its control-register loop (`$F84D6E`) |
| Ramsey `$DE0003`, Gary `$DE0000/1` | register file (read-back) | bonus polls read-back (`$F84D8C`) |
| autoconfig `$E80000` | any constant | `ReadExpansionRom` (`$F84942`) rejects er_Reserved03 ≠ 0 or manufacturer 0/$FFFF |
| `$A00000` | not $91 | step 3 |
| `$F00000` | not $1111 (unless used as a hook, §3.3) | step 2 |
| `$C00000–$D9FFFF`, page after container fast RAM, `$07F00000` | open bus, writes discarded | memory probes (step 13, §3.4) |

Unexpected values:

| Condition | Result |
|---|---|
| ROM checksum ≠ $FFFFFFFF | `COLOR00` $F00, LED blinks, `reset` (`$F804AE` → `$F80EF4`): reset loop |
| vector RAM test fails | $0F0, same loop |
| any exception after step 8, before exec's vectors (bus error on an unmapped probe) | handler `$F804AA`: $FE5, same loop |
| `InitCode` returns | $F0F, `bra .` (`$F80406`) |
| frozen beam, BBUSY stuck, CIA TA not counting, TOD not ticking (ROM timer.device) | silent hang |
| Ramsey `$DE0003` not read-back and `$DE0043` ≠ $7F | hang in supervisor |

## 3. Warm reset, KickTags, ROM scan

### 3.1 ExecBase checks (`$F80262`, identical in all five images)

1. `$4` even.
2. `ChkBase` (`$26`) = ~ExecBase.
3. 16-bit sum of words `$22…$52` (SoftVer … ChkSum) = $FFFF.
4. If `ColdCapture` (`$2A`) ≠ 0: cleared, called in supervisor with return address in A5.
5. `KickMemPtr`/`KickTagPtr`/`KickCheckSum` (`$222–$22A`) and Cold/Cool/WarmCapture are carried into the new ExecBase. `lib_Version` = 47 only decides whether private `$20E` is carried.

Nothing else is validated (not `LowMemChkSum`, not `MaxLocMem`). If check 2 or 3 fails, expansion.library re-checks the same pointer after autoconfig (`$F842C4`), for an ExecBase in board RAM.

In exec's own init (`$F80594` → `$F80F1C`), after expansion:

| Step | Address | Rule |
|---|---|---|
| KickCheckSum | `$F8107C` | $FFFFFFFF + every long of each KickMem `MemList` (header + entries: 4 + 2·`ML_NUMENTRIES`) + every RomTag pointer in the KickTag table (link entries with bit 31 set not summed); must equal `KickCheckSum`. Same algorithm as `SumKickData()` |
| KickMem | `$F810D0` | `AllocAbs` of every `MemEntry`; any failure ⇒ all KickTags ignored |
| KickTags | `$F81054` | table of RomTag pointers; 0 ends; bit 31 set = link to next table. RomTags are not checked for $4AFC/self pointer |

### 3.2 RomTag scan and replacement

Range table (pointer at file `$588` A1200/A500, `$4FC` A3000, `$560` A4000/A4000T): `$F80000–$F840C0`, `$F80000–$1000000`, `$F00000–$F80000`. **`$E00000` is not scanned; `$F00000–$F7FFFF` is**, in every image. Scan (`$F80F8A`): word $4AFC with `RT_MATCHTAG` = its own address, continue at `RT_ENDSKIP`.

Insertion (`$F80FC4`), in order ROM ranges → KickTags: a tag whose name (case-sensitive) is already listed replaces it if its **version is higher, or equal with priority ≥**; otherwise it is dropped. The list is kept sorted by priority. So a KickTag or `$F00000` tag with the same name, version 47 and the ROM module's priority replaces it without changing init order.

Init passes: `InitCode(RTF_SINGLETASK)` runs from the ROM-only array built at `$F803F6`; exec's init builds the merged array and then starts `InitCode(RTF_COLDSTART)` itself (`$F80896`) and never returns. **A KickTag or `$F00000` module with `RTF_SINGLETASK` never initialises.** COLDSTART modules run in user mode; `RTF_AFTERDOS` ones when dos calls `InitCode(RTF_AFTERDOS)`.

### 3.3 Other hooks

| Hook | When | Mode |
|---|---|---|
| `$1111` at `$F00000` | right after the ROM checksum, before any hardware write; return via `jmp (a5)` | supervisor, SP = `$400`, no vectors |
| ColdCapture | step 9, before chip RAM sizing | supervisor |
| CoolCapture | before `InitCode(RTF_COLDSTART)` | user |

### 3.4 Where KickMem can live

Allocation between steps 12 and 15 is first-fit from the bottom of chip RAM (`$4000` up) and of fast RAM; KickMem must lie above that: the top of chip RAM, as `LoadModule` does. Fast RAM at `$08000000` is added by expansion.library (A1200, A3000, A4000 images: `$F84320`, attributes $105, priority 40) **before** the KickMem `AllocAbs`, so KickMem may also be there; the A500/A600 image has no such probe. The probe walks 1 MB steps until a write does not read back; the page after container fast RAM must read as open bus, not fault.

## 4. Idle loop

`$F815AC`: `move.w #$2700,SR`; if `TaskReady` is empty: `IdleCount`++, `SysFlags` bit 7 set, **`stop #$2000`**, repeat. No busy loop. The dispatcher around it (`$F81596–$F81622`) does per switch: two SR writes, `move usp` twice, `rte`, and with an FPU (`ex_LaunchPoint` = `$F81746`) `fsave`/`frestore` and a format $9 frame.

## 5. timer.device 46.1

| Resource | Use | Address |
|---|---|---|
| CIA-A TB | continuous from $FFFF (`crb` = $11): E-clock counter with ICR bit 1 vector; read hi/lo/hi under `Disable` | `$FC6160`, `$FC6A8E` |
| one of CIA-B TA, CIA-A TA, CIA-B TB | one-shot interval timer; table of (timer, control) addresses at `$FC697E`, chosen through a hook in `ciaa.resource` | `$FC6924` |
| CIA-A TOD + alarm | calibration and ICR bit 2 vector | `$FC6124`, `$FC6094` |
| VERTB server | UNIT_VBLANK | `$FC6080` |
| `ciaa.resource` `AddICRVector` bits 2, 0, 1 | | `$FC6094–$FC60C0` |
| `battclock.resource` | system time at init | `$FC6176` |

- **Calibration** (`$FC61D4`): waits for a TOD tick, runs TB one-shot until the next tick, and derives the power-line rate: remaining ≥ $CCFC ⇒ 60 Hz, ≥ $B5DA ⇒ 50, ≥ $99F8 ⇒ 30, else 25. Stored in `PowerSupplyFrequency` (`$213`). Without TOD ticks it loops forever.
- **E-clock**: `ex_EClockFrequency` (`$238`) = 715909 (data `$FC6366`), 709379 if graphics set `REALLY_PAL` (`$FC60DC`). A replacement must set both ExecBase fields.

## 6. graphics.library init

Sequence (A1200 image; same code in all):

| Address | Access | Result |
|---|---|---|
| `$F8EFEA` | `VPOSR` bits 14–8 | Agnus ID; bit 5 ⇒ HR_AGNUS |
| `$F94998` | `INTENA` ← $0009, 17× `DENISEID` with `INTENAR` reads between | stable ID or $FF (OCS) |
| `$F8F050` | `BPLCON3` ← $C00, `BPLCON4` ← $11, `DENISEID` | Lisa revision |
| `$F8F160…$F8F266` | `DMACON` ← $0080, `COP1LC`, `COP2LC`, sprite pointers | copper lists in chip RAM |
| `$F8F26C` | `BLTCON0/1`, `BLTSIZE` ×2, `BLTSIZV` | dummy blit |
| `$F8F346` | CIA-B `crb` bit 7, `todhi` | — |
| `$F8F362` | wait until beam line in 20…160 (`Disable`d) | needs moving beam |
| `$F8F378` | `BPLCON0` ← $102 (ERSY), 375 `VHPOSR` reads, compare H | moving ⇒ GENLOC |
| `$F8F3B0` | CIA-A TOD ← 0, TA one-shot $FFFF; poll TA hi | TOD tick within ≈46 ms ⇒ TODA_SAFE |
| `$F8F434` | PAL from ID bit 4 (ECS/AGA) or from line count > 270 (OCS) | PAL ⇒ `REALLY_PAL`, VBlankFrequency 50 |
| `$F8F5CC` | `DMACON` ← $81A0, `INTENA` ← $0040 | — |

`SetChipRev` (LVO −888, `$F90AEC`) adds AA_ALICE if ID bit 1 is set and AA_LISA if the Denise probe returns bit 2 clear ($F8).

**No-chipset answer**: all-zero, static registers hang at `$F8F362`. The minimum is a moving beam counter, BBUSY = 0, a counting CIA-A TA, `INTREQ` software-set interrupts and VERTB. With those, graphics initialises as OCS; AGA answers (§2 table) give the real machine's display database. Copper lists and the blank view live in chip RAM; nothing reads them back.

## 7. scsi.device

| Image | Probe | Hardware |
|---|---|---|
| A1200, A500/600 | Gayle ID via execPrivate18 (`$FB750E`); 0 ⇒ no IDE | `$DA2000` IDE |
| A3000 | SDMAC | `$DD0000` |
| A4000 | IDE | `$DD2020` |
| A4000T | both of the above, plus `NCR scsi.device` (53C710 at `$DD0040`, Gary `$DE0000` timeout) | |

What the ROM driver does at init (A1200 `$FB5214`), which a replacement must reproduce:

1. Opens expansion (execPrivate17(6)), `AllocConfigDev`; fills `cd_BoardAddr`, `er_Type` = $10 (ERTF_DIAGVALID) and a DiagArea pointer in `er_Reserved0c` (`$FB5274`).
2. DiagArea at `$FB519C`: `da_Config` $90 (word-wide, DAC_BOOTTIME), `da_BootPoint` +$0E: `FindResident("dos.library")`, jump to its `RT_INIT`. `AddBootNode` needs this ConfigDev for a bootable node before dos runs (`expansion.doc`).
3. Per unit: RDB scan ('RDSK' `$FB8694`, 'PART' `$FB87B0`, 'FSHD'/'LSEG' `$FB899C`/`$FB8A5E` into `FileSystem.resource`), `MakeDosNode` (`$FB8814`), `AddBootNode(de_BootPri, ADNF_STARTPROC, dn, cd)` (`$FB8858`); cd is the ConfigDev only when the PART has PBFF_BOOTABLE, else NULL with priority −128.
4. Commands: `CMD_READ/WRITE`, TD64 (24–27), NSD `$C000–$C003`, `HD_SCSICMD` (28) (`$FB5950–$FB5974`); HDToolBox needs `INQUIRY`, `READ CAPACITY`, `MODE SENSE` through `HD_SCSICMD`.

## 8. Running in a user-mode process

No PMMU instruction (`pmove`, `pflush`, `ptest`) occurs in any image. Privileged instructions by site:

| Instruction | Where | Frequency |
|---|---|---|
| SR writes, `move usp`, `rte` | dispatcher, `ExitIntr`, interrupt handlers, Supervisor routines | every switch/interrupt |
| `stop #$2000` | idle `$F815C0` | idle |
| `movec VBR` | probe `$F80D16` (VBR ← 0; temporary illegal/F-line handlers during the probe) | boot |
| `movec CACR` | probe `$F80D3C` ($A09: bit 9 sticks ⇒ 030, bit 0 sticks ⇒ not 040), `$F807A8`, `CacheControl`; utility.library toggles bit 14 (`$FC14D8`) | boot, cache calls |
| `movec DTT0/DTT1/ITT0/ITT1` | 040 path of the probe: DTT0 = $0000C040 (`$00xxxxxx` non-cacheable), others $00FFC000 | boot |
| `cinva`, `cpusha` | probe, `CacheClearU/E` | cache calls |
| `fsave`/`frestore` | FPU probe, FPU task switch | every switch with FPU |
| `reset` | `ColdReboot` `$F80F04` (skipped when exec runs below `$200000`) | guest reboot |

Assumptions to honour:

- `Supervisor()` (`$F80CBC`) executes `ori #$2000,SR` in user mode and relies on the privilege-violation handler (`$F80CD4`) finding PC = `$F80CBC` in a format 0 frame: reflected frames must be exact.
- Exec keeps tasks in user mode from step 18; supervisor code runs only in traps, interrupts and `Supervisor()`. `GetCC` becomes `move ccr,d0` on 010+ (`$F80746`).
- AttnFlags come from CACR read-back: `vCACR` must implement the host CPU's writable-bit mask. A 68060 host reads as 040 + FPU40 until `68060.library`; `CACR` $A009 at `$F80D80` sets 060 EIC and FIC.
- The supervisor stack is allocated by exec (`$1800` bytes, `$F80650`) in guest RAM.

## 9. Consequences for the design

What [guest-container-design.md](guest-container-design.md) takes from this analysis:

1. **§6.1 `containerinit`**: `RTF_COLDSTART`, priority 106–109 (before `diag init` 105), not `RTF_SINGLETASK`. It runs in user mode.
2. **§6.1 replacements**: same name, version 47, the ROM module's priority (ties go to the later tag); keeps init order. Add `NCR scsi.device` (A4000T) to the replaced set; drop the card.resource stub (it self-disables without Gayle). Add `diag init`, `A3000/A4000 bonus`, `syscheck`, `ramdrive.device`, `syslog`, `system-startup` as keep. The timer replacement must set `ex_EClockFrequency` and `PowerSupplyFrequency`.
3. **§6.1/§2.5 fast RAM**: map container fast RAM at `$08000000`; the ROM's expansion.library adds it (A1200/A3000/A4000). `containerinit` adds it only for the A500/A600 image.
4. **§6.2 injection**: the ExecBase stub needs only §3.1 fields; `$0` ≠ 'LOWM'/'HELP'; KickMem at the top of chip RAM or in `$08000000` fast RAM; `KickCheckSum` = `SumKickData()`. Answer `$DE0002` bit 7 = 0 on A3000/A4000 images.
5. **§6.2/§2.5 extension area**: move it from `$E00000` to **`$F00000–$F7FFFF`**, scanned by every image; same replacement rule, no ExecBase stub, survives guest resets. The first word must not be $1111, or it becomes an early hook. Consider making it the default over KickTags.
6. **§6.4 registers**: add the §2 answer table: Alice/Lisa or ECS IDs per image, beam frozen under ERSY, CIA-A TA/TB counting and TOD ticking at the E-clock, software-set `INTREQ` raising the level, Gayle `$DE1000` constant, Ramsey `$DE0043` = $7F with `$DE0003` read-back, `$DE0002` bit 7 = 0, autoconfig any constant.
7. **§5 boot-phase windows**: add `$A00000` (PCMCIA attribute), `$BFA000` (CIA-A alias page), `$C00000–$D9FFFF`, `$F00000` (if no image), `$07F00000` (A3000/A4000) and the page after fast RAM, all open bus with writes discarded (not zero RAM: a sticky write is detected as memory).
8. **§5 decoder**: long accesses spanning two custom registers (`move.l $DFF004,d0`, `move.l a0,$DFF080`) and RMW forms on CIA registers (`bset`/`bclr`/`andi.b`/`ori.b` on `crb`).
9. **§2.2 CPU model**: `vCACR` read-back mask per host CPU; record TT registers; `fsave`/`frestore` in the list of hot sites (FPU task switch); privilege-violation reflection with the exact faulting PC.
10. **§2.4/§5 hot sites**: 160 inline 8-byte `INTENA` writes in the A1200 image beyond exec's `Disable`/`Enable`; the dispatcher (`$F81596–$F81622`) is the next target. **Any ROM patch breaks the reset checksum** (§2 step 1): patches must be undone before the guest's reset reaches `$F80152`, and load-time patches must rewrite the fix-up long at file `$7FFE8`.
11. **§10.5 idle**: exec idles with `stop #$2000`.
12. **§16 risk 2**: the failure modes are known (§2 table); only the listed probes run before exec's vectors, and each wrong answer is a reset loop or a silent hang.

Open (not visible statically): the one-shot timer chosen by timer.device on a given boot; whether graphics' VBL server needs `VPOSR` LOF toggling; `NCR scsi.device` behaviour with open bus if left in place.
