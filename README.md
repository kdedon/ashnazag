# Ash Nazag - The One Distribution

One SVR4 kernel and userland for 68k Macintosh, Atari and Amiga machines, built on
Commodore's AMIX 2.1 and the community AMIX 68040/68060 port. Other 68k systems run on
top of it as environments: ordinary Unix processes with their own display session.

## Hosts

| Host | Emulator | Real hardware | Status in the emulator |
|---|---|---|---|
| Quadra 800 (68040) | QEMU `q800` with a Quadra 800 ROM | Tested | Boots from disk to a multi-user login on the built-in screen and ADB keyboard. SCSI, SONIC Ethernet, clock, swap, ufs, loadable modules, display and sound services, X11. All environments below. |
| Falcon030 (68030, no FPU) | Hatari (TOS 4.04) | Tested; slow | Boots from IDE to a login on VGA or RGB monitors. X11 and xdm, TOS, CP/M and System 6 environments. |
| CT60/CT63 Falcon (68060) | Hatari 060 with FastRAM | In progress | Kernel boots from FastRAM; SuperVidel or Videl console. TOS environment (EmuTOS, XaAES). |
| Amiga A3000, A4000 (68040, 68060) | Amiberry (Mediator, Voodoo3, CyberStorm PPC) | Untested | Started from AmigaDOS by `unix_boot040`; login on the console. Root on the A3000's SCSI, the A4000's IDE or a CyberStorm PPC/MK III's SCSI. PCI through openpci.library (Prometheus, Mediator), NE2000 Ethernet, ZZ9000 RTG, Voodoo3 display with 3D (X and 8-, 16- and 32-bit Workbench); Radeon 9200 driver written, not yet run on a card. Mac, TOS and Amiga environments; the 68060 runs Motorola's integer package for the instructions it lacks. |

## Environments

One start command per family, run as an ordinary user: `-e NAME` picks an environment,
`-l` lists them, `-d NAME` sets your default. Ctrl-Option-Command-0 shows the console,
-1 to -9 the sessions. Details: [docs/environments.md](docs/environments.md).
The test suite runs in QEMU on the Quadra; tests on the Amiga and Falcon hosts, and the
environments missing there, are tracked in #243.

| Environment | Command | Hosts | Status |
|---|---|---|---|
| Mac System 7.6.1 | `startmac` | Quadra, Amiga | Works: Finder, applications, Open/Save, CommandShell, sound, Log Out and Shut Down. |
| Mac OS 8.1 | `startmac -e 8.1` | Quadra | Works: Finder, applications, Open/Save, sound through A/UX's Sound Manager, Open Transport networking. |
| Mac System 6.0.7 | `startmac -e 6` | Quadra, Falcon | Works, in colour; slow. Needs A/UX 2.0.1 and a IIci ROM. |
| TOS (EmuTOS, TeraDesk) | `starttos` | Quadra, Falcon, Amiga | Works: EmuTOS's own BIOS and XBIOS, patched to call host services; no Atari chips modelled. fVDI at the screen's size; XaAES with `starttos -X xaaes` (or `desktop=xaaes` in `~/TOS/.env`). YM2149 and DMA sound on host sound; NVRAM in `.nvram` in C:. |
| Amiga (Kickstart 3.x, Workbench) | `startamiga` | Quadra, Amiga (68040 or 68060) | Partial: Workbench on RTG at 8, 16 or 32 bits where the host has them; Warp3D on a Voodoo3 (16-bit screens), else in software (slow); 640x480 and 800x600 game screens; 3D survives session switches; slow start; no chipset display. |
| Concurrent CP/M-68K | `startcpm` | Quadra, Falcon | Works, in the terminal: CP/M 3 BDOS, XDOS processes, more consoles with `startcpm -c N`, RSXes. Z80 and 8086 programs too. |
| Windows 3.x programs (Win16) | `startwin` | Quadra | Works for Program Manager and the tested programs. Windows for Workgroups 3.11 by default, Windows 3.11 with `-e win311`. Each user installs their own Windows and Wabi 2.2 with `startwin -install`. |
| Apple Lisa (Office System 3.1) | `startlisa` | Quadra | Partial: your Lisa ROM boots your ProFile volume to the Office desktop at any screen size. The ProFile is read only, writes go to a file beside it. |
| TI Voyage 200 (AMS 2.09, 3.10) | `startvoyage200` | Quadra | Works: your Flash dump boots to the AMS desktop; archived variables are TI variable files on the host; OFF/ON. |
| NeXTSTEP / OPENSTEP | `startnext` | Quadra | In progress: your OPENSTEP install boots to the Workspace Manager, drawn 1:1 at the screen's size (8-bit gray), with sound, login panel and text console; no network yet. |
| X11R6.3 | `startx`, `xsession` | Quadra, Falcon, Amiga | Works: twm, XView, xdm session chooser; sessions for each environment. OpenGL through Mesa 5, in software or on a Voodoo3 through DRI; `glxgears`. |

Software Quake runs on the display service; the game data is yours. The display service
lists the host's modes and lets a session request one; a new mode is kept only if Return
is pressed within 15 s ([docs/display-modes.md](docs/display-modes.md)). Environments still start at the host's size. SMSQ/E, other TI
calculators, TT and more are planned (`docs/*-plan.md`).

## What you need

Nothing proprietary is in this repository and nothing proprietary is ever distributed.
You supply:

- the AMIX 2.1 tape archive (both parts) and its 2.1 patch disk
- Mac: Quadra 800 and Quadra 700 ROMs, the A/UX 3.1 CD, Mac OS 7.6.1 and 8.1 CDs; System 6 needs the A/UX 2.0.1 CD and a IIci ROM
- Falcon: TOS 4.04 for passthrough (EmuTOS is fetched)
- Amiga environment: a Kickstart 3.0 or later (an Amiga host's own is used), the AmigaOS 3.2 CD and Picasso96; Amiga host: its Kickstart ROM and openpci.library
- CP/M: optionally Digital Research's CP/M-68K 1.3 release zip, for its programs (AS68, ED, STAT, ...); the system itself is ours
- Win16, at run time: Windows 3.1 or 3.11 and Wabi 2.2
- Lisa: your Lisa boot ROM and a ProFile image with the Office System installed
- Voyage 200: a Flash dump of your calculator
- NeXT: your NeXT ROM and an OPENSTEP CD

`tools/setup.sh` builds the toolchains and fetches the open-source parts; see `BUILDING.md`.

## Build

```
sh kernel/build.sh                      # Quadra kernel: kernel/build/unix-mac.elf
GROUP=smoke sh tests/run-qemu.sh        # tests in QEMU q800; groups: smoke core mac tos mint cpm amiga display sound win16 gl quake stable
sh images/macenv/mkmacimage.sh          # Quadra disk image with every environment
PLATFORM=atari sh kernel/build.sh       # Falcon030 kernel (PLATFORM=atari060: CT60)
sh kernel/atari/mkdisk.sh               # Falcon disk image
PLATFORM=amiga sh kernel/build.sh       # Amiga kernel
sh kernel/amiga/test/mkhdf.sh           # Amiga disk
```

`BUILDING.md` lists the media each target needs; `sh tools/check-env.sh` reports what is
missing. [docs/hosts.md](docs/hosts.md) covers booting each host; [docs/kernel-memory.md](docs/kernel-memory.md)
the kernel's memory.

Each push to main refreshes the
[`unstable`](https://github.com/kdedon/ashnazag/releases/tag/unstable) prerelease: kernel
modules for mac, amiga, atari030 and atari060 as SVR4 packages (`pkgadd -d FILE all`).
Userland and environment packages are built locally.

## Layout

| Path | Contents |
|---|---|
| `kernel/mac/` | Quadra 800 platform layer: boot, MMU, SCC, SCSI, console, ADB, SONIC, RTC, display and sound services |
| `kernel/atari/` | Falcon030 and CT60 platform layer, loader, disk image, Hatari tests |
| `kernel/amiga/` | Amiga host: amilib (AmigaOS libraries in the kernel), openpci, PCI and RTG drivers |
| `kernel/dlm/` | SVR4.2-style loadable kernel modules, disk controller drivers included ([docs/modules.md](docs/modules.md)) |
| `kernel/guest/` | environments: A/UX personality and Mac (`mod/`), TOS (`tos/`), Amiga (`amiga/`), CP/M (`cpm/`), Win16 (`win16/`), Lisa (`lisaboot/`, `mod/lisaguest/`), Voyage 200 (`voyage200/`), NeXT (`next/`) |
| `kernel/otbridge/` | Open Transport bridge for Mac OS 8 networking |
| `kernel/drm/` | DRM for 3D cards (Voodoo3) |
| `gl/` | Mesa, DRI drivers, Glide3 and Quake builds |
| `x11/` | X11R6.3 server, clients, xdm and session scripts |
| `images/` | disk-image builders and QEMU scripts |
| `tests/` | functional test suite run under QEMU |
| `webapp/` | disk image forge: builds images in the browser from your media |
| `tools/` | COFF/ELF converter, setup and media tools |
| `docs/` | design documents |

## Built on

- [amix-040-060-port](https://github.com/asokero/amix-040-060-port): 68040/68060 support for AMIX
- [gcc-cross-amix](https://github.com/isoriano1968/gcc-cross-amix): AMIX cross toolchain
- [x11r6.3-amix](https://github.com/isoriano1968/x11r6.3-amix): X11R6.3 for AMIX, base of our server
- [amigaux.org](https://amigaux.org/): AMIX packages, zz9000-amix
- [amix-gayle-ide](https://github.com/hmlio/amix-gayle-ide): the A4000 IDE driver
- [EmuTOS](https://emutos.sourceforge.io/), fVDI, TeraDesk, XaAES: the TOS environment
- [FreeType](https://freetype.org/): TrueType for the Win16 environment (fetched at build time)
- [Mesa](https://mesa3d.org/) 5.0.2: OpenGL (fetched at build time)
- XFree86 4.3 tdfx DRI driver and Glide3: Voodoo3 3D (fetched at build time)
- [quake-amix](https://github.com/asokero/quake-amix): Quake for AMIX (fetched at build time; game data not included)

## AI disclosure

Most of the code and documentation here was written with AI assistance (Claude, by
Anthropic), under the maintainer's direction. Every change is reviewed and run through
the test suite before it is merged; hardware results are reported separately from
emulator results.

## Licence

MIT (see `LICENSE`). Third-party components keep their licences; see `THIRD_PARTY.md`. GPL code ships only as separate programs, patches or separately loaded modules, never linked into a kernel image.
