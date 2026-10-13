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

## Host drivers

| Module | Host | Role |
|---|---|---|
| `asc` | Mac | `/dev/asc` (46): Apple Sound Chip for the sound service |
| `auxsnd` | Mac | `/dev/snd`: A/UX Sound Manager device, relayed to the sound service |
| `dmasnd` | Atari | `/dev/dmasnd` (46): Falcon DMA sound |
| `aen` | Atari | `/dev/aen0` (18): NetUSBee Ethernet |
| `scsi`, `sd`, `dpn` | Atari | SCSI core and adapters; disks; DaynaPORT or BlueSCSI Ethernet (`dpn0`) |
| `amilib` | Amiga | AmigaOS libraries in the kernel (exec, expansion, utility, dos, mmu, timer.device) |
| `opci` | Amiga | openpci.library on amilib: the PCI bus behind Prometheus, Mediator and similar bridges |
| `pci` | Amiga | `/dev/pci`: configuration space, for `lspci` |
| `ne` | Amiga | `/dev/ne0`: RTL8029 (NE2000) on PCI |
| `zz9000` | Amiga | `/dev/zz9000`: MNT ZZ9000 RTG and its display-service screen |
| `voodoo3` | Amiga | PCI Voodoo3 or Banshee as the display-service screen |
| `tdfx` | Amiga | `/dev/dri/card0`: DRM for the Voodoo3's 3D |
| `radeon` | Amiga | `/dev/dri/card1`: DRM for the Radeon's 3D |
| `floppy` | Amiga | `/dev/floppyN` (62): raw tracks for the floppy service |

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
