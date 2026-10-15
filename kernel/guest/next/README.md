# NeXT

`native` runs the user's OPENSTEP `mach_kernel` (below) or, given a 128 KB NeXT turbo boot ROM, the ROM
from its reset vector, as native 68k code with the MMU off. The nextguest module supplies the
CPU-side board only: system and interrupt registers, timers, clock chip and NVRAM, and a 2-bit
1120x832 display drawn centred on the host's 8-bit screen. Chip and controller models (SCSI, DMA,
desktop bus, ethernet) are not provided: those devices come as drivers over host calls.

    sh kernel/guest/next/build.sh OUTDIR     # OUTDIR/native

Per environment (`~/NeXT/<env>`): `mach_kernel` or `media` (the ROM image, 128 KB), `nvram` keeps the
clock chip's settings, `vram` is the screen. RAM is the largest of 32, 16 or 8 MB that
free memory holds. Cmd-Q quits.
`NEXT_CENSUS=1` prints each register's first access on stderr.

Tested with `GROUP=next sh tests/run-qemu.sh` (the ROM from `media/NeXTROMS.zip`).

    native -k ~/NeXT/ENV/mach_kernel         # the user's kernel instead of the ROM

`-k` loads the kernel's m68k slice (a Mach-O, fat or thin) into RAM, builds the boot
frame it reads from its entry stack (monitor globals zeroed, one RAM bank, empty argument
string) and enters it with the MMU off; no ROM is needed. The monitor's services are host
services: the kernel's message ring is copied to this program's output on every timer tick
(its address comes from the kernel's symbols), its "back to the monitor" trap vector points at
a store to a doorbell, which ends the run and prints the command line the kernel passed, and
the desktop-bus driver's start routine (named in the symbols) is replaced by a return in the
loaded copy, since no controller answers its reset. Without the ROM monitor a bank's end
leaves 8 KB for the kernel's message ring. `tools/makenext.sh ENV` copies
the OPENSTEP CD into `~/NeXT/ENV`.

## Host filesystem (root)

The root is `~/NeXT/<env>` served by `hfsguest.c` (code that runs in the guest kernel)
and `hfshost.c` (the host side); `hfswire.h` is the mailbox. `native -k` maps a region at
`HF_BASE`, copies the code in, hands it the kernel's own routines and adds it to the first free
filesystem-switch entry as type `hfs`, and replaces the loaded kernel's root-device setup
(`_setconf`) with a store of that name into `_rootfs`, so the root mount selects the entry
and calls its mount-root operation with no device probe and no network code; the boot
device name is `hf`. Owners, modes and device nodes come from `~/NeXT/<env>.meta`.

Found in the kernel by symbol name: `_setconf` (entry replaced by a jump), `_rootfs` (the root type name, 4 bytes), `_vfssw` and
`_vfsNVFS` (the switch: 8-byte entries, name then operations; the root mount looks the name up by
string compare and calls operation 6 of the entry), `_kalloc`, `_vm_info_init`, `_uiomove`, `_vfs_add`, `_vfs_unlock`,
`_bcopy`, `_active_u` (the current cred is at word 26 of it, uid at +2, gid at +4).

Layouts relied on (Sun-style VFS): vfs operations are mount, unmount, root, statfs, sync,
vget, mountroot; vnode operations are open, close, rdwr, ioctl, select, getattr, setattr,
access, lookup, create, remove, link, rename, mkdir, rmdir, readdir, symlink, readlink,
fsync, inactive, then bmap, strategy and the rest. A vnode has the VM info pointer first,
flag at +4, count at +6, operations at +28, vfs at +36, type at +40, rdev at +44, private
data at +46. Directories are read as 1024-byte blocks of `ino, reclen, namlen, name`.

Where it stops: after the device probe the kernel starts its threads on kernel stacks it
allocated from its own virtual address space (around 0x10df3000, outside RAM); with the MMU
off the first switch to one faults, before the root mount runs. The translation registers
the kernel sets are recorded but not applied.
