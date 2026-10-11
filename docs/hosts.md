# Hosts: build and boot

Media each step needs: `BUILDING.md`. Proprietary media (ROMs, A/UX, AMIX, Mac OS, TOS,
AmigaOS) are yours; no image built here may be redistributed.

## Quadra 800

```sh
sh kernel/build.sh                      # kernel/build/unix-mac.elf
sh images/macenv/mkmacimage.sh          # images/work/x11/q800-mac.img
```

`q800-mac.img` holds the disk root, X11 and every environment whose media you have.
Smaller images: `images/mkboot.sh` (kernel only, RAM root) and
`kernel/mac/diskroot/mkdiskimage.sh` (disk root); see `images/README.md`.

QEMU (`toolchain/qemu-local`, patched; stock QEMU loses 68040 page writes):

```sh
qemu-system-m68k -M q800 -m 128 -bios Q800.ROM \
  -drive file=q800-mac.img,format=raw,if=none,id=hd0 -device scsi-hd,scsi-id=0,drive=hd0 \
  -nic user
```

Real machine: write the image to the whole disk at SCSI ID 0 (`dd`, see `images/README.md`).
The ROM boots it directly; no Mac OS or A/UX Startup is involved. Log in on the screen as
`guest` or `root` (no passwords). Ctrl-Option-Command-0 shows the console, -1 to -9 the
display sessions.

## Falcon030 and CT60

```sh
PLATFORM=atari sh kernel/build.sh       # kernel/build/unix-atari030.elf
PLATFORM=atari060 sh kernel/build.sh    # kernel/build/unix-atari060.elf (CT60/CT63)
TOSENV=1 MACENV=1 X11=1 sh kernel/atari/mkdisk.sh [kernel.elf [out.img]]
```

The image (default `kernel/build/atari/disk/falcon-disk.img`) is an AHDI disk: loader, root
(ufs), swap and `/home`; `GEM=1` makes the last partition a TOS FAT drive instead. TOS 3 or
4 boots it from power-on. Options are listed at the top of `mkdisk.sh`.

Hatari: `kernel/atari/build-hatari.sh` builds the patched Hatari;
`kernel/atari/run-hatari.sh -d COPY.img -r tos404.img` runs it (`-c 060 -t MB` for a CT60).
Real machine: write the image to the IDE or CompactFlash disk with `dd`.

Ctrl-Alt-0 shows the console, Ctrl-Alt-1 to -9 the sessions. `bootline` shows or sets the
kernel command line. Network: set `ADDR` and `NETIF` (`aen0` NetUSBee, `dpn0` DaynaPORT or
BlueSCSI) in `/etc/inet/network-config`.

## Amiga

```sh
PLATFORM=amiga sh kernel/build.sh       # kernel/build/unix-amiga
sh kernel/amiga/stage.sh                # environment modules: kernel/build/amiga/package
PKG=kernel/build/amiga/package UNIXBOOT=unix_boot040 OPENPCI=DIR \
  sh kernel/amiga/test/mkhdf.sh [kernel [outdir]]
```

`mkhdf.sh` makes `amix.hdf` (Amiga RDB, SCSI unit 6: root and swap) and `boot/`, an
AmigaDOS directory whose Startup-Sequence starts the kernel with `unix_boot040`. You
supply `unix_boot040` and openpci.library.

Amiberry: `KICK=a3000.rom sh kernel/amiga/test/amiberry.sh outdir` runs an A3000 with a
68040, a Prometheus PCI bridge with an NE2000, and a ZZ9000. The login is on the serial
port (TCP 1234). Real hardware is untested.
