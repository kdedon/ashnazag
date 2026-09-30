# Quadra device tables and disk root policy

Validated statically and by host tests; not yet run on a Mac.

## Device tables

`cdevsw`, `bdevsw` and `io_start` are weakened in the base and redefined in `macdevsw.c`,
as `coinfo` is. Reasons for overriding the tables rather than each driver with `nodev`:

- **Size.** It would take about 55 overrides against 3 here. The 13 Amiga cdev rows and 2 bdev
  rows name 49 distinct entry points and 4 streamtabs, and each one would need its own stub.
- **Entries `nodev` cannot cover.** Several Amiga rows open with `nulldev` (6 `amiga`,
  12 `machid`), so stubbing the opens is not enough: the read, write, ioctl and mmap entries
  would all need stubs too. The STREAMS rows (5, 13, 18, 46) are data, so each needs a
  replacement streamtab.
- **Collateral.** Renaming a function also redirects any internal Amiga callers of that name.
- **Matches the base model.** The tables are what `master.d` generates, so a Mac table is a Mac
  configuration, and a table dump shows the result directly.

**Constraints.**

- The majors and sizes stay the base's: `cdevsw[70]` is 0xE38 bytes and `bdevsw[32]` is 0x400.
  `shadowcsw`, `shadowbsw`, `cdevcnt` and `bdevcnt` come from the base. `relink-mac.sh` checks the
  sizes.
- Kept rows name the same symbols as the base row. `devtab/verify.sh` compares them against the
  base's `.rela.data`.
- `coinfo` and `ram*` bind to the Mac objects by name.

`fmodsw` is all generic, so it is not overridden.

## Major table

**cdevsw.** All other majors (7, 38, 39, 42–45, 47–49, 51–69) are empty in both the base and the
Mac table.

| Major | Base driver | Fate | Why |
|---|---|---|---|
| 0 | console (`coinfo`) | kept → SCC | Mac `coinfo` |
| 1 | prf | kept | kernel profiler, no hardware |
| 2 | tty (`sy*`) | kept | generic |
| 3 | mem (`mm*`) | kept | generic |
| 4 | bb | **empty** | billboard: captures `conputc`, which is the Amiga screen `coputc` |
| 5 | sl (`slinfo`) | **empty** | Paula serial, CIA/custom |
| 6 | amiga (`am*`) | **empty** | Amiga custom-chip / chip-RAM access |
| 8, 9 | xt, sxt | kept | layers, STREAMS |
| 10 | screen (`scr*`) | **empty** | Amiga display (copper, 0xDFFxxx) |
| 11 | scsi (`gsioctl`) | kept | pass-through over the Mac `sdqueue` |
| 12 | machid (`miread`) | **empty** | answers "A2500/A3000" and probes RAM below MAINSTORE |
| 13 | ql (`qlinfo`) | **empty** | 6551/6502 multiport serial board |
| 14, 15 | pts, ptmx | kept | generic |
| 16 | ct | kept | SCSI tape over the Mac `sd.h` layer |
| 17 | fd | **empty** | Amiga floppy (CIA, custom DMA) |
| 18 | aen (`aeninfo`) | **empty** | A2065 Ethernet |
| 19, 20 | slip DLPI, loop | kept | STREAMS, no hardware |
| 21 | par | **empty** | CIA parallel port |
| 22 | tiga | **empty** | TIGA Zorro graphics |
| 23–30 | timod, tirdwr, log, sp, clone, ticots, ticotsord, ticlts | kept | generic |
| 31 | res | **empty** | Resolver Zorro graphics |
| 32–37 | ip, tcp, udp, rawip, icmp, arp | kept | generic |
| 40 | dd (raw) | kept | SCSI disk over the Mac `sd.h` layer |
| 41 | ben | **empty** | battery clock at 0xDC0000 |
| 42 | — | empty statically | `mac_rd_config()` installs the raw RAM disk at boot |
| 46 | audio (`audioinfo`) | **empty** | Paula audio |
| 50 | sad | kept | generic |

**bdevsw.** Majors 0–15 and 21–31 are empty.

| Major | Base | Fate | Why |
|---|---|---|---|
| 16 | fd | **empty** | Amiga floppy |
| 17 | hd | **empty** | A2090 ST-506 disk |
| 18 | dd | kept | root/swap disk (Mac SCSI underneath) |
| 19 | dum | kept | dummy disk: no hardware, fails I/O with a message |
| 20 | ram | kept → Mac `rd.o` | RAM disk |

**Other tables.**

| Table | Contents | Status |
|---|---|---|
| `io_init` | `parinit` | Mac stub |
| `io_poll` | `qlintr`, `slpoll` | Mac stubs |
| `io_start` | `{ mac_diskprobe }` | Mac (empty in the base) |
| `io_halt` | empty | — |
| `init_tbl` | fpuinit … tcooinit | generic |
| `fmodsw` | module ldterm sockmod app ttcompat timod tirdwr ptem pckt slipmod connld | generic |
| `int2_tbl`, `vbinttab` | Amiga ISR chains | referenced only by the weakened Amiga `p2int`/`p3int` bodies. Unreachable |
| vectors (`M68Kvec`) | autovectors 25–30 → Mac `p1int`…`p6int` | others go to `nullvect` / FPSP |

`oncons()` compares `d_open` against `scropen`'s address. No row holds `scropen`, so only
`coinfo` (major 0) counts as the console.

## `config_orig`

- The port's `stext` calls `config_cachefix`, which ends in `jmp config_orig`.
- `config_orig` is the base's second global name for its Amiga `config`; weakening `config`
  does not redirect it. Left alone, `stext` would run the Amiga config, which writes INTENA at
  0xDFF09A (RAM on a Mac).
- `cfgorig.s` overrides `config_orig` with `jmp config`, so the Mac `config` runs after the
  port's cache fix (whose `btrace_*` calls are no-ops while `btrace_on` = 0).
- `devtab/verify.sh` checks the chain (the "config chain" lines).
- Any overlay must check whether a weakened base symbol has a second global name at the same
  address. Among the current 34 overrides, `config_orig` is the only one.

## Reachability check (`checkimg.py`)

**The graph.**

- A reference graph over the linked ELF. Nodes are every text/data symbol, plus the weakened
  base bodies, marked dead.
- Edges come from annotated operands, from immediates loaded or pushed as addresses, from
  fall-through, and from data words equal to a symbol (the vector table included).
- Compare operands and string literals do not count as edges. The embedded RAM image is not
  scanned.

**What it checks.**

- It flags any node reachable from `mac_entry` that uses an absolute address in
  0xA00000–0xFFFFFF (CIA, custom chips, clock, Zorro II autoconfig).

**Expected result:**

- No weakened body is referenced.
- None of the 53 emptied entry points or streamtabs is reachable.
- The only reachable Amiga-address code is the port's `btrace_mark` and `btrace_hex` debug
  trace. It writes Amiga colour registers only when `btrace_on` ≠ 0, and `btrace_on` is 0 in
  the image.

## Root and swap policy

Evaluated in `config()`. The first case that applies wins. Minors are dd minors: target in
bits 0–2, slice in bits 4–6.

| # | Case | root | swap = dump |
|---|---|---|---|
| 1 | `root=cNd0sM` (M 1 or 3–7) on the command line | cNd0sM | cNd0s2 |
| 2 | RAM-disk image | rd0 | rd1 |
| 3 | none of the above | **halt** | — |

A disk is never root without `root=`: the A/UX hand-off picks (launch `-e`/`-p`, A/UX's root
disk) and c0d0s1 may be A/UX's own file systems. `mac_diskpick` still decodes them; `config()`
prints the candidate before halting. `root=` on slice 0 (whole disk) or 2 (swap) is ignored.

**Hand-off (`mac_diskpick`).**

- `mac_auxinfo[0]` is the physical address of the `'Pigs'` block. It is read with the MMU off,
  before `pstart`. It must lie below `stext` and carry the magic.
- **Field offsets.** From A/UX `sys/module.h` `struct kernel_info`:

  | Offset | Field |
  |---|---|
  | 0x88 | `root_ctrl` |
  | 0x8A | `root_drive` |
  | 0xB6 | `ki_flags` |
  | 0xB9 | `root_partition` |
  | 0xBA | `swap_ctrl` |
  | 0xBC | `swap_drive` |
  | 0xBD | `swap_partition` |

- **Rules.** As in A/UX `/unix` `choose_root` (0x58bb4):
  - The partition fields are used only when `ki_flags` & 8 is set. Otherwise root is partition 0
    and swap is partition 1 of the root disk.
  - A/UX makes its root device from `root_ctrl` (the SCSI ID), `root_drive` and the partition.
- **A/UX partition → slice.** A/UX partition *n* is our slice *n*+1, the same +1 the partition-map
  scan uses (`bzb` slice numbers, root → s1, swap → s2).
- **Rejected values.**
  - Target > 6 (7 is the host).
  - Drive ≠ 0 (dd has no LUN).
  - Partition > 6.
  - An invalid root drops the hand-off entirely.

**Probe (`mac_diskprobe`, `io_start`).** It runs just before `vfs_mountroot` and only when root is
on dd.

- **Same slice.** If root and swap/dump are the same slice, it panics before anything is written.
- **Root file-system type.**
  - It opens the root slice. `ddopen` reads the partition map.
  - It reads the s5 superblock (block 1, `s_magic` 0xfd187e20 at +504), then UFS (block 16,
    `fs_magic` 0x011954 at +1372).
  - It sets `rootfstype` to `s5` or `ufs`.
  - If neither is found, `rootfstype` stays empty and `vfs_mountroot` tries every vfssw entry.
- **Swap size.**
  - It opens the swap slice (`dumpdev`).
  - It sets `swapfile.bo_size` to `ddsize()` minus `bo_offset`, replacing the fixed 61440.
  - If the slice does not open, it prints `swap … missing`. `swapconf` will then panic, as it
    does in the base.
- **Why before the mount.** The common snode's size (`commonvp` → `d_size`) is taken when
  `swapconf` looks the name up. The swap disk's map is therefore already loaded, even when swap
  is on a different disk from root. `swapadd`'s own clamp needs a known size.

Every offset is in `macroot.h`. `offchk.c` fails the build if the AMIX headers disagree.

## Host tests

`verify.sh` runs `test/hosttest.c`, which covers:

- `mac_diskpick`: no block, bad magic, block past `stext`, implicit targets 0/3/7/−1, LUN 1,
  other flag bits, explicit root/swap pairs, partition 7, invalid swap → root disk s2.
- `mac_dskname`.
- `mac_fsprobe`: blank, read error, synthetic UFS, synthetic s5, and the RAM-disk `root.img`
  (detected as s5).
- `mac_root`, cut from `macconf.c`: case order, `root=` beating `-e/-p`, and a bad `root=`
  falling to the default.

## Sources

- AMIX: the link kit's `master.d/kernel.c` and `filesys.c`; `sys/*.h`, `vm/bootconf.h`,
  `sys/fs/*`; driver sources in `amiga/driver`, `amiga/alien` and `local/res.c` (used to
  classify drivers); disassembly of the base and the linked image.
- A/UX: shipped header `sys/module.h`; `/unix` (`choose_root`, `kernelinfoptr` users);
  `docs/mac-boot-and-memory-map.md`.

## Open items

1. **The flag bit is untested on a real launch.** The meaning of the bits in `ki_flags`
   (`KI_VERSION_VALID` = 8 set by `-e`/`-p`) comes from the header and the launch notes.
   Confirm with `launch -d`.
2. **A missing swap slice still panics** in `swapconf`, as in the base. There is no no-swap mode.
3. **Swap follows `dumpdev`.** The probe takes the swap device from `dumpdev`, which `mac_root`
   always sets to the swap slice. If they are ever separated, add a variable.
4. **The probe opens the disk before the root mount.** `sdopen`'s bus reset and 2 s settle
   therefore happen in `io_start`, and the mount reuses the initialised chip. The probe uses
   only `ddopen`/`ddsize`/strategy.
5. **`rootfstype` is `s5` for any s5 block size.** AMIX registers only `s5` in vfssw, so 512 B,
   1 KB and 2 KB filesystems all get `s5`. A/UX SVFS/UFS mountability under AMIX is not
   established.
6. **Stubs.** `parinit`, `qlintr` and `slpoll` are function stubs. The `io_init`/`io_poll`
   arrays could be overridden like `io_start` instead.
7. **Amiga console code is unreachable, not removed.** `conputc`/`congetc` still hold the Amiga
   `coputc`/`cogetc`, but nothing live calls through them: `bb` is empty and the Amiga `config`
   is bypassed.
8. **`machid` (12) is empty.** An installer that reads `/dev/machid` gets ENODEV.
