# TOS environment design

`starttos` boots the user's EmuTOS unmodified, natively in user mode with a virtual
supervisor state (guestcore). No chip is modelled and no access is decoded: the I/O
windows are pages of constants, and every routine that needs a live answer is patched
in the loaded copy to call a host service.

## Host calls

A patch is a `jsr` to a stub in the launcher, which runs in the same process. The
stub saves the registers, moves to a host stack and calls the service, which reads and
sets the guest's registers; guestcore holds interrupts while the pc is in the
launcher's code. This is the Lisa environment's mechanism, moved to a shared header
so every environment uses one entry and one register layout:

	patched routine --jsr--> stub n --> hostcall(r[], n) --> rts

An XBIOS call is replaced in the dispatch table. When TOS also calls the routine
itself, its entry jumps to the stub as well, unless another table slot shares it.
The cartridge reaches the same stubs through a table in its parameter block. Services
that need the module (interrupts, display state) use `/dev/tos` ioctls.

## Finding routines

Nothing is found by address, build or checksum. Entry points come from structures
every EmuTOS has:

- the BIOS and XBIOS dispatch tables: the trap handlers' `move.w count,d1; lea
  table,a0` pair, the BIOS one followed by a branch into the shared tail;
- the exception vectors EmuTOS sets, read after its own initialisation (VBL $70,
  Timer C $114, ACIA $118);
- short instruction sequences unique to one routine (below).

A ROM in which a required routine is not found is refused at start, with its name.

## Routines

| Hardware | Routine | Found by | Becomes |
|---|---|---|---|
| any absent or present chip | `check_read_byte` (all machine, RAM, sound, NVRAM, IDE, blitter, cartridge-space probes) | `move.l sp,d1; move.l $8.w,a1; lea (pc),a0; move.l a0,$8.w; moveq #0,d0` | host call: present or absent from the environment's machine table (constants) |
| MFP Timer C, 200 Hz | `int_timerc`, set up by `xbtimer(2, ...)` | vector $114 | host timer raises level 6 vector $114 |
| | its in-service clear | `move.b #$df,$fffffa11.w` (or `bclr #5`) before `rte` | `clr.l` of the module's in-service flag: Timer C is not taken again until then, as on the MFP |
| VBL | `int_vbl` | vector $70 | host timer raises level 4 |
| MFP timers A, B, D | `xbtimer` | XBIOS 31 | timers A and B: host timer at the requested rate, vector from the MFP table; D (delay calibration) never fires, so EmuTOS keeps its default loop counts |
| MFP enables | `jdisint`, `jenabint` (and `mfpint`, which calls them) | XBIOS 26, 27 | host calls: the module's enable mask; a disabled channel drops what it owed |
| IKBD ACIA | `kbd_int` input | Kbdvbase | unchanged: the cartridge hands host events to TOS's handlers each VBL; events wait in its queue while the VBL queue is off |
| IKBD ACIA | `ikbd_writeb` | XBIOS 25 (`ikbdws`) and its callee | unchanged: TX empty is a constant, writes are ignored |
| IKBD ACIA | `ikbd_readb` (reset wait) | its ACIA status poll and 1 ms delay loop | host call: no byte |
| MIDI ACIA | `bconout3`, `bconin3` | BIOS device table | constants: TX empty, nothing received (as today) |
| RTC / NVRAM / IKBD clock | `gettime` | XBIOS 23 | host call: host local time, GEMDOS format |
| | `settime` | XBIOS 22 | unchanged: no RTC is present, its IKBD write is ignored; GEMDOS keeps the time it is given |
| | `nvmaccess` | XBIOS 46 | host call: `.nvram` in the environment's folder (memory only with `-i` or a read-only C:); the RTC half stays absent, so the clock is `gettime`'s |
| Shifter palette | VBL copy from `colorptr` (`Setpalette`) | `lea $ffff8240.w,a1; moveq #7,d1` | host call: the 16 colours into the screen state; returns past the copy loop |
| | `setcolor`, `esetcolor`, `esetpalette`, `egetpalette`, `esetbank` | XBIOS 7, 83, 84, 85, 82 | host calls: the palette lives in the screen state; reads come back from it |
| Shifter mode and base | VBL `screenpt` store | `move.l d0,$44e.w; lsr.w #8,d0` | host call: the base into the screen state; returns past the base register writes |
| | `setscreen`, `esetshift` | XBIOS 5, 80 | original runs (it keeps the Line-A and system variables), then a host call sets base and mode |
| | `physbase`, `getrez`, `egetshift`, `esetgray`, `esetsmear` | XBIOS 2, 4, 81, 86, 87 | host calls: answered from the screen state |
| | the screen set at boot | the VBL's `jsr detect_monitor_change` after `addq.l #1,$462.w` | host call: on the first VBL, base and mode from `_v_bas_ad` and `sshiftmod`, the ROM's reset palette; the monitor never changes |
| Videl | `vsetmode`, `vsetrgb`, `vgetrgb` | | unchanged: no Videl is present, EmuTOS answers them itself |
| YM2149 | `giaccess`, `ongibit`, `offgibit`, `dosound` | XBIOS 28, 29, 30, 32 | host calls: registers in shared memory; the sound pump plays them on a host sound stream and runs `dosound` sequences at 50 Hz; reads return the last value set. Synthesis is an approved exception to "no chip models" |
| | Timer C's Dosound step (bell, key click set its sequence directly) | `move.l d2,-(sp); movea.l sndtable,a0; cmpa.w #0,a0; beq`, then YM writes | host call: the sequence goes to the pump |
| Videl | the boot's Falcon probe (`tst.b $ffff8282.w` under a bus error handler) | `lea (pc),a0; move.l a0,$8.w; tst.b $ffff8282.w` | jumps to its handler: no Videl, so no Falcon memory set-up |
| Microwire (STE/TT mixer) | its shift-register wait at sound init | `move.w $ffff8922.w,d0; bne.s` to itself | the wait ends at once; writes are ignored |
| DMA sound | XBIOS 128-141 | the cartridge's XBIOS hook (`xsnd.c`) | unchanged: host sound service |
| FDC, ACSI | `timeout_gpip` (`flop_detect_drive`, `send_command`) | its loop: GPIP bit 5 test branching back to a `_hz_200` compare | returns "timed out" at once: `nflops` 0, no ACSI units |
| IDE, SCSI, SCC, NVRAM, blitter, DSP | probes | `check_read_byte` | absent |
| ST-RAM, TT-RAM size | `memconf`, TT-RAM probe | `check_read_byte`; memvalid magic | the launcher sets `memvalid`/`memval2`/`ramtop` so the warm-boot path skips sizing |
| 68030 MMU | `pmove`, `pflush`, `ptest` at boot | privileged instructions | guestcore's 68030 PMMU answers |
| cartridge | `$FA0000` magic | memory | the machine-layer cartridge, as today |

The I/O window ($FF8000-$FFFFFF, aliased at $FFFF8000) is three pages of the RAM
object ($FF8000, $FF9000, $FFF000), written with constants at start and then mapped
read-only: an MFP GPIP with no interrupt asserted and the monitor bit for `-M`, ACIA
status TX empty, DMA status no error, the initial shifter mode, video counters 0.
The writers that run after boot are patched. Any other write faults; the module
counts it with its pc and maps a copy of the page there for the instruction, which
the next interrupt delivery drops. The tos tests assert no such write after boot. The display process shows the screen state the host calls
keep (base, TT mode and bank, 256 TT colours; the ST palette is the current bank)
in the cartridge's shared page. The rest of the window is not mapped; an access there is a hardware-page
fault, counted by the module and delivered as a bus error. The tos tests assert the
count stays 0 in steady state.

ST-RAM is a module RAM object mapped at 0 and again at $FF000000, its 24-bit
alias. The ROM's alias ($FFE00000) is not mapped: an access there is a bus error.

## Kept

fVDI, XaAES, `-M`, `-S`, Cubase, the MiNT shim, host-folder drives and STiK are
unchanged: they already reach the host through the cartridge, as does `-i` (C: from
a FAT image the user names). `-P` (the machine's own TOS on its
hardware) is removed; real-TOS passthrough is a separate, later feature.

## Open

- The FDC and ACSI probe timeouts cost boot time; if they do, `flop_detect_drive`
  and the ACSI scan become host calls answering "no drive".
- Programs that write video, palette or sound registers directly (games, demos)
  get no effect, as on the Amiga; the GEM and XBIOS paths are complete.
