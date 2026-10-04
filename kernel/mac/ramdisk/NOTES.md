# RAM-disk root

## Files

| File | Role |
|---|---|
| `rd.c` | block + raw RAM-disk driver, `mac_rd_config()` root/swap glue |
| `build.sh` | `build/rd.o`, `build/rdimage.o` (image as `.data`, symbols `rd_image`/`rd_image_end`) |
| `mks5fs.py` | s5 image builder from a manifest |
| `s5check.py` | independent reader: lists, extracts, checks consistency |
| `root.manifest`, `etc/` | contents of the root; `etc/` holds the files written here |
| `mkroot.sh` | extracts tape segment 02 to `build/core`, builds and checks `build/root.img` |

`build/` holds AMIX binaries and the image: local only.

## Design

**Driver.** Takes over the stock Amiga RAM disk's names (`ramopen` … `ramsize`), which the
kit's `bdevsw[20]` already references, so the relink weakens only five more symbols. Two
units: 0 = root image, 1 = swap. Strategy copies with `bcopy` 2 KB at a time, translating the
buffer address with `vtop(addr, b_proc)` as the AMIX disk drivers do. Page-I/O buffers are
safe: `fix_swtbls` wraps every block driver without the no-breakup flag in `gen_strategy`,
which hands the driver a physical address per page; the kernel is identity-mapped below 1 GB,
so physical addresses are usable directly. Raw I/O goes through `uiophysio` with
`B_READ`/`B_WRITE`. The raw row (`cdevsw[42]`, empty in the kit) is filled at config time
because the table lives in the base image; `cdevsw` rows with flag 0 are not wrapped by
`fix_swtbls`, so the late fill is safe.

**Image source.** A boot-record `BI_RAMDISK` (Linux/m68k tag 6, {addr, size}) wins over the
linked-in image. It must lie above the page-rounded `end` and below 1 GB; `VSIZOFMEM` is cut
below it so the VM never hands out its pages (memory above it is lost; loaders put it at the
top of RAM). The embedded image is kernel `.data`, already reserved.

**Swap.** AMIX needs a swap device: `swapconf` panics if `swapfile.bo_name` does not look up,
and u-areas are reserved against swap (`segu_get`). `anon_resv` reserves all anonymous memory
against both swap (`ani_max`) and `availsmem`, so swap size caps it. Unit 1 is half the memory
above `end` + 4 MB, 2 MB (`RD_SWAPKB`) to 32 MB (`RD_SWAPMAXKB`), carved off the top of the
kernel's memory chunk at config time (no BSS, no image cost).

**Root/swap selection.** `config()` writes `rootdev`, `dumpdev`, `rootfstype` and `swapfile`
(all `.data` in the base) at run time, leaving the base image and the port's stamping tools
unchanged.

**Filesystem.** s5, as AMIX's own root; `s5` is first in the kernel's `vfssw`, and `ufs` is
linked too. 1 KB blocks (`s_type` 2). Layout facts, all checked against the linked kernel:

| Fact | Source |
|---|---|
| `struct filsys` offsets (`s_fsize` 4, `s_free` 12, `s_inode` 214, `s_time` 420, `s_tfree` 432, `s_state` 500, `s_magic` 504, `s_type` 508) | AMIX headers compiled with the AMIX cross compiler; kernel compares `s_magic` at 504, switches on `s_type` at 508 |
| clean state: `s_state + s_time == FsOKAY`; root mounts anyway, marked `FsBAD` if not | `s5mountroot` |
| superblock at byte 512; inode *i* at block `2 + (i-1)/INOPB`, 64-byte `dinode` | `s5mountroot`, `iread` |
| `di_addr`: 13 × 3-byte big-endian; 10 direct, single, double, triple indirect | `iread`/`iupdat` |
| devices: `addr[1] |= 1`, `addr[2]` = major, `addr[3]` = minor (EFT), `addr[0]` = old 16-bit form | `ialloc`, `iread` |
| free chunk block: `int df_nfree; daddr_t df_free[50]` | `s5fblk.h` compiled |

The free list is built as classic mkfs does (free block 0 first as terminator, then highest to
lowest), so allocation starts at low blocks. Inode 1 is a mode-`IFREG` placeholder so `ialloc`
never hands it out. All times are fixed (`-t`), so the image is reproducible.

**Contents** (1 MB image): `/sbin/init`, `/sbin/autopush` with `/etc/ap/chan.ap` and
`/dev/sad/{user,admin}` (50,0/1), run from `inittab` as `sysinit` so the console gets `ldterm
ttcompat`; `/sbin/sh` (+ `/usr/bin/sh` symlink); `/usr/lib/libc.so.1` (the only shared object:
both `PT_INTERP` and `NEEDED` of every binary here); `mount`/`umount` (+ `/usr/lib/fs/proc/mount`
for `ps`), `sync`, `uadmin`, `uname`, `mknod`, and `ls cat echo date ps pwd cp rm mkdir ln chmod
stty sleep od ed env id wc head tail sed true false kill`; `/etc/inittab` = `is:s:initdefault:`,
`/etc/profile`, `TIMEZONE` (GMT0), `ioctl.syscon`, `passwd`, `group`, empty `mnttab` and
`utmp[x]`/`wtmp[x]`; `/dev` console/syscon/systty (0,0), tty, mem, kmem, null, zero (used by the
dynamic linker), RAM-disk nodes. `/etc/sulogin` is a hard link to `/sbin/sh`: SVR4 `init` does
not fall back to `sh` without it. Single user is a root shell with no password.

**Console modes.** `init` applies `/etc/ioctl.syscon` (termio as 13 hex fields: iflag, oflag,
cflag, lflag, line, cc[8]) when it opens the console. The Mac Delete key sends BS, so ours sets
erase ^H, kill ^U, intr ^C, quit ^\, eof ^D; `ECHOE` so BS erases on the screen; `IGNBRK` in
place of `BRKINT`, so noise on an unconnected modem port cannot send SIGINT. Otherwise as the
tape (9600 8N1, ICRNL, IXON/IXANY, ONLCR, TAB3). `/etc/profile` repeats the `stty` for a login
shell.

## Checks

- `mkroot.sh` runs `s5check.py`, which walks the tree from inode 2 and checks `.`/`..`, link
  counts, that every block is claimed once by data, indirect or free list, no lost blocks,
  `s_tfree`/`s_tinode` and the free-inode cache. Extracted files compare equal to the tape
  copies. A corrupted `s_tfree`, link count or shared block is each reported.
- After relinking, the kernel link should show 0 unresolved symbols, `bdevsw[20]` bound to
  `rd.o`, and the embedded image byte-identical in the ELF. `rd.o` has no BSS.

## Sources

AMIX link-kit headers and `master.d/kernel.c`, the kit's Amiga driver sources (`ram.c`, `dd.c`,
`physdsk.c`, `sdpart.c`) for driver conventions. The s5 layout otherwise follows the published s5 format
description.

## Open questions

- The console node assumes the SCC driver sits at `cdevsw[0]` minor 0.
- Swap on RAM only satisfies `swapconf` and anon reservation, at the cost of the RAM it takes.
  With less than 4 MB + 256 KB above `end`, no swap unit is made and `swapconf` panics.
- `gen_strategy` behaviour for multi-page page-I/O is inferred from the port's notes and the
  kernel's `fix_swtbls`, not traced line by line.
- `/proc` is mounted at boot from `vfstab`; stock `mount` dereferences a null `FILE` when `/etc/vfstab` is missing.
- A boot-record RAM disk below the top of the kernel chunk wastes the memory above it.
