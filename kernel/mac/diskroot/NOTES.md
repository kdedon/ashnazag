# Disk root and multi-user login

`q800-unix-disk.img` boots the Quadra 800 into our kernel with root on `c0d0s1` (ufs), swap on
`c0d0s2`, run level 2 and a `login:` prompt on the screen console.

## Files

| File | Role |
|---|---|
| `mkdiskroot.sh` | builds `build/root.img` from the tape and `root.manifest` |
| `mkdiskimage.sh` | builds `out/q800-unix-disk.img` (runs `mkdiskroot.sh`, `images/mkboot.sh`, `addparts.py`, checks) |
| `root.manifest`, `etc/` | contents of the root; `etc/` holds the files written here |
| `fstree.py` | manifest and SVR4 cpio reader shared by the builders |
| `mkufs.py`, `ufscheck.py` | ufs image builder; independent reader and checker |
| `mks5fs.py`, `s5check.py` | s5 builder and checker (`ROOTFS=s5`) |
| `addparts.py` | appends the `Apple_UNIX_SVR2` partitions and their map entries |
| `test/fstest` | in-guest file-system round trip (ufs and s5 on spare slices) |

`build/` and `out/` hold AMIX binaries and the image: local only.

## Disk layout

512-byte blocks; 104,906,752 bytes, about 21 MB allocated (sparse).

| Blocks | Map entry | Slice | Contents |
|---|---|---|---|
| 0 | DDM | s0 | whole disk, 204,896 blocks |
| 1–63 | 1 `Apple_partition_map` | — | 5 entries |
| 64–95 | 2 `Apple_Driver` | — | A/UX disk's driver, byte-identical |
| 96–8287 | 3 `Apple_HFS` "MacOS" | s4 | boot blocks, kernel `unix`, command line `root=c0d0s1` |
| 8288–139359 | 4 `Apple_UNIX_SVR2` "Root" | s1 | ufs root, 64 MB |
| 139360–204895 | 5 `Apple_UNIX_SVR2` "Swap" | s2 | swap, 32 MB |

Root and swap carry A/UX bzbs as A/UX 3.1 writes them: root `ABADBABE`, type 1, flags 0xC000
(root + usr); swap type 3, flags 0x2000. The kernel's partition scan assigns s1 and s2 by bzb
role; `mkdiskimage.sh` checks this by building `scsi/test/apmtest.c` on the host.

## File-system choice: ufs

AMIX installs its root as s5 and the kernel mounts either (the probe reads the superblock).
The disk root is ufs; `ROOTFS=s5` builds an s5 root instead. s5 on a disk slice relies on the
`uiomove` wrapper (`s5dir/S5DIR.md`) to keep directory updates; `tests/s5disk` covers it.

ufs parameters, as `mkfs -F ufs special SIZE 32 16 8192 1024 16 10 60 4096 t 0 0`: 8 KB blocks,
1 KB fragments, 16 groups of 16 cylinders (4 MB), 960 inodes per group (15,360), 10% free, no
rotational delay. `mkufs.py` writes the same superblock and group structures as AMIX's `mkfs`;
an empty 64 MB file system differs from an AMIX-made one only in which of two blocks holds `/`
and which `/lost+found`. `ufscheck.py` accepts AMIX-made file systems and catches single-byte
damage to a group map, a link count, the totals, the clean state or a block count.

## What is in the root

Tape segments 02 (core), 03 (bsd) and 10 (terminfo; compiled `vt100` included), as the cpio
archives list them: owners, modes, times, hard links and device nodes kept. Changes:

| Path | Change | Why |
|---|---|---|
| `/stand/*` | Amiga kernel and boot files removed; `/stand/unix` = our kernel ELF | namelist for `ifconfig`, `crash` |
| `/dev/scr`, `screen`, `term/*`, `dsk/fd*`, `rdsk/fd*`, `cage`, `clock`, `aen*`, `amiga`, `machid`, `noise`, `par`, `tiga*`, `res*` | removed | majors the Quadra tables leave empty |
| `/dev/term/b` | new, 0,1 | SCC printer port (the modem port is the console) |
| `/etc/inittab` | own | run level 2; console login `getty console console`; no Amiga `sioc`/`sysinit` |
| `/etc/sysinit` | own | `bcheckrc` (fsck -m, mnttab, /proc, /dev/fd), node name |
| `/etc/ap/chan.ap`, `/etc/ioctl.syscon`, `/etc/TIMEZONE` | as the RAM root | ldterm/ttcompat on the console, Mac keys, GMT |
| `/etc/nodename` | `q800` | |
| `/etc/vfstab` | root line `ufs` | |
| `/etc/profile` | `TERM=vt100` on the console, no `sioc` | the screen console parses vt100 |
| `/etc/screendefs` | entries removed | `ttymon` configured Amiga screens from it |
| `/sbin/shutdown` | no `sioc` | |
| `/sbin/rc0`, `rc6` | `sync; sleep 5` after `umountall` | delayed writes reach the disk before `uadmin` halts |
| `/usr/sbin/swap` | page shift 12 for 11 (5 bytes) | `-l`/`-s` counted 2 KB pages; this kernel's are 4 KB |
| `/usr/amiga/bin/setclk` | `exit 0` | `date` runs `setclk -s`; the kernel sets the RTC |
| `/etc/saf/_sactab` | empty; port-monitor directories removed | Amiga screens, serial boards, tcp/inetd/xdm |
| `/etc/inet/network-config` | empty | it probed the Amiga Ethernet board; loopback still comes up |
| `/var/adm/utmp`, `utmpx` | added, empty | |

`/dev` keeps the tape's nodes for the kept majors: disks `c0..f d0..1 s0..7` (block 18, raw 40;
minor = target | slice << 4), console/syscon/systty, tty, mem/kmem/null/zero, ptmx and 32 `pts`,
`sad`, `log`, clone nodes (ip, tcp, udp, …), xt/sxt, tape `rmt` (16), `scsi`, `prf`, `fd`.
Run level 2 starts `sac` (no port monitors), `cron`, loopback (`lo0`), and the console `ttymon`.

## Login

`root`, no password (the tape's `/etc/shadow` has none; `/etc/default/login` has `PASSREQ=NO`).
Set one with `passwd`. Other tape accounts are unchanged (no passwords, except `games`, `news`,
`x`, `nobody`, `noaccess`, which are locked).

## Building

```sh
sh kernel/mac/diskroot/mkdiskroot.sh TAPE_DIR   # once; copies segments 02 03 10 to build/tape
sh kernel/mac/diskroot/mkdiskimage.sh           # -> out/q800-unix-disk.img
```

`mkdiskimage.sh [kernel.elf [out.img]]` rebuilds the root with that kernel as `/stand/unix`,
then the boot disk (`images/mkboot.sh -c root=c0d0s1`, which needs `images/q800-test-small.img`
and rebuilds `bootblk/build`), then appends the partitions. Checks: `ufscheck.py`, `auxsash.py
apmcheck`, `mkbb check`, the kernel's partition scan, and slice 1 read back equal to the root
image. Variables: `ROOTMB` (multiple of 4), `SWAPMB`, `ROOTFS=s5`, `SPARE="24 24"` (spare slices
s4, s5 for `test/fstest`; HFS then becomes s6). Output is reproducible for a given kernel.

## Testing

Tests run under QEMU q800 with the Quadra 800 ROM and 32 MB, via `images/qemu/run-mac.sh`
(`-snapshot`). They cover:

- boot: the log shows s1 "Root" and s2 "Swap" from their bzbs, `mac: root c0d0s1 ufs, swap
  /dev/dsk/c0d0s2`, "The system is ready." and `q800 login:` on screen and serial;
- login as `root` and `ls`, `df -k`, `ps -ef`, `swap -l`, `who -r`, `ifconfig lo0`;
- persistence: write files, `shutdown -y -g0 -i0`, `hmp:commit all` into a copy of the image,
  check that copy with `ufscheck.py`, boot it again and read the files back; `fsck -n` clean;
- `test/fstest` on spare slices (`SPARE="24 24"`): the same operations on ufs and s5, then
  `fsck -n` and a data compare after remount.

## Limits and open points

- **Restart:** `init 6` restarts through the ROM to `login:`; `init 0` stays halted
  (`rtc/NOTES.md`).
- **Clock:** time comes from the RTC (GMT when PRAM holds no Map offset); `date` sets it
  (`rtc/NOTES.md`).
- `netstat`, NFS/RFS and the lp service (segments 07/05) and sysadm (segment 09, compressed) are
  not included.
- `/usr/amiga` (part of core) is present but nothing runs from it.

## Sources

AMIX tape binaries and scripts (copied or edited with `sed` at build time; none stored here);
AMIX headers (`sys/fs/ufs_fs.h`, `ufs_inode.h`, `ufs_fsdir.h`), with offsets taken by compiling
them with the AMIX cross compiler; the published 4.2/4.3BSD FFS layout; the shipped kernel's
`uadmin`/`s5mountroot`/`bdwait`; file systems made by AMIX `mkfs` as references.
