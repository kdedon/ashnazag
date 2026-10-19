# Loadable modules and host services

Modules are SVR4.2-style DLM files (`docs/dlm-spec.md`) in a `mod.d` directory. At run
level 2, `/etc/rc2.d/S05aux` registers them (`auxreg`, `otreg`, `amigareg`); each loads on
the first open of its device. `modadmin` lists, loads and unloads by
hand.

## Environments

| Module | Device (major) | Depends on | Role |
|---|---|---|---|
| `guestcore` | | | guest processes: virtual supervisor state, fault frames, guest RAM, host ROM mapping, sockets |
| `auxcore` | | guestcore | A/UX system calls and shared libraries |
| `auxexec` | | auxcore | exec of A/UX COFF programs |
| `uinter` | `/dev/uinter0` (54) | auxcore | Mac environment: screens, events, cursor, PRAM |
| `tosguest` | `/dev/tos` (56) | guestcore | TOS environment |
| `amigaguest` | `/dev/amigaguest` (57) | guestcore | Amiga environment |
| `cpmguest` | `/dev/cpm` (59) | guestcore | CP/M-68K environment |
| `otbridge` | `/dev/otbridge` (55) | | Open Transport bridge for Mac OS 8 |
| `dsuser` | `/dev/fb0` (51), `/dev/kbd` (52), `/dev/mouse` (53) | | display service devices: frame buffer and input |

## Boot modules

Disk controller drivers are modules. The host's boot loader reads them from the boot area and passes their addresses in the boot record (`<sys/bootmod.h>`). The kernel links them before the root mounts; they attach to the disk driver with `sd_attach`. Without a boot module, the first disk open loads the host's controller module from the module path once a root is mounted.

| Module | Host | Loader |
|---|---|---|
| `ncr96` | Mac | the HFS boot blocks (`images/mkboot.sh -k`, default `ncr96`) |
| `amigaide` | Amiga | A4000 IDE port, card 0: `unix_boot040` from `S:Modules` (mkhdf.sh `BOOTMODS`) |
| `ncr770` | Amiga | CyberStorm PPC / MK III SCSI, card 1: as `amigaide` (`ROOTBUS=ncr770`) |
| `ataide` | Atari | Falcon IDE, card 0: the AXB boot sector's module table (mkdisk.sh `BOOTMODS`, default `ataide scsi sd`) |
| `scsi`, `sd` | Atari | SCSI core and disks, card 1: as `ataide` |

The A3000's and A2090/A2091's WD33C93 drivers and the Atari's AHDI partition layer stay in the kernel.

## Host drivers

| Module | Host | Role |
|---|---|---|
| `asc` | Mac | `/dev/asc` (46): Apple Sound Chip for the sound service |
| `auxsnd` | Mac | `/dev/snd`: A/UX Sound Manager device, relayed to the sound service |
| `dmasnd` | Atari | `/dev/dmasnd` (46): Falcon DMA sound |
| `aen` | Atari | `/dev/aen0` (18): NetUSBee Ethernet |
| `dpn` | Atari | DaynaPORT or BlueSCSI Ethernet (`dpn0`) over `scsi` |
| `amilib` | Amiga | AmigaOS libraries in the kernel (exec, expansion, utility, dos, mmu, timer.device) |
| `opci` | Amiga | openpci.library on amilib: the PCI bus behind Prometheus, Mediator and similar bridges |
| `pci` | Amiga | `/dev/pci`: configuration space, for `lspci` |
| `ne` | Amiga | `/dev/ne0`: RTL8029 (NE2000) on PCI |
| `zz9000` | Amiga | `/dev/zz9000`: MNT ZZ9000 RTG and its display-service screen |
| `voodoo3` | Amiga | PCI Voodoo3 or Banshee as the display-service screen |
| `tdfx` | Amiga | `/dev/dri/card0`: DRM for the Voodoo3's 3D |
| `radeon` | Amiga | `/dev/dri/card1`: DRM for the Radeon's 3D |
| `floppy` | Amiga | `/dev/floppyN` (62): raw tracks for the floppy service |

## Filesystems

A module of type `fs` claims one of 8 spare `vfssw` rows. A mount of an unknown type loads the module of that name from the module path; it cannot unload while mounted.

| Module | Role |
|---|---|
| `cdfs` | read-only CD: ISO 9660, Rock Ridge, Joliet, UDF, HFS, HFS+, audio tracks. `mount -F cdfs card,unit /cdrom` (SCSI adapter index, target id, e.g. `0,3`); the disk root ships `/usr/lib/fs/cdfs/mount` and `/cdrom` |

## Services

- Display (`docs/display-service-design.md`): `dsuser`'s devices. Each
  environment and X gets a session; only the session in front sees input. Sessions are
  chunky (8, 16 or 32-bit) or planar (Amiga bitplanes, 1-4 planes). The hot keys switch sessions.
- Sound: `sndd` serves `/dev/sound` on the Mac's sound chip or the Falcon's DMA sound.
  Only the front session is heard; others are drained at the output rate so their
  programs keep time. Mac environments use it through `/dev/snd`, TOS through its XBIOS
  sound calls, the Amiga environment through AHI.
- Floppy: on Amiga hosts, `floppy` shares the drives with AMIX's own driver; the Amiga
  environment's trackdisk.device uses them or ADF files. Other hosts have none yet.
