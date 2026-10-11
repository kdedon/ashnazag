# Ash Nazag - The One Distribution

One SVR4 kernel and userland for 68k Macintosh, Atari and Amiga machines, built on
Commodore's AMIX 2.1 and the community AMIX 68040/68060 port. Other 68k systems run on
top of it as environments: ordinary Unix processes with their own display session.

## Hosts

| Host | Tested on | Status |
|---|---|---|
| Quadra 800 (68040) | QEMU `q800` with a Quadra 800 ROM; real Quadra 800 | Boots from disk to a multi-user login on the built-in screen and ADB keyboard. SCSI, SONIC Ethernet, clock, swap, ufs, loadable modules, display and sound services, X11. All environments below. |
| Falcon030 (68030, no FPU) | Hatari (TOS 4.04); real Falcon | Boots from IDE to a login on VGA or RGB monitors. X11 and xdm, TOS, CP/M and System 6 environments. Slow on real hardware. |
| CT60/CT63 Falcon (68060) | Hatari 060 with FastRAM | Kernel boots from FastRAM; SuperVidel or Videl console. Real-hardware testing in progress. |
| Amiga A3000 (68040, 68060) | Amiberry; WinUAE for PCI 3D cards | Started from AmigaDOS by `unix_boot040`; login on the console. PCI through openpci.library (Prometheus), NE2000 Ethernet, ZZ9000 RTG, Voodoo3 detected. Real hardware untested. |

## Environments

Start commands, run as an ordinary user. Ctrl-Option-Command-0 shows the console,
-1 to -9 the sessions. Details: [docs/environments.md](docs/environments.md).

| Environment | Command | Hosts | Status |
|---|---|---|---|
| Mac System 7.6.1 | `startmac` | Quadra | Works: Finder, applications, Open/Save, CommandShell, sound, Log Out and Shut Down. |
| Mac OS 8.1 | `makemac -e 8.1`, then `startmac -e 8.1` | Quadra | Works: Finder, applications, Open/Save, sound through A/UX's Sound Manager, Open Transport networking. |
| Mac System 6.0.7 | `startmac6` | Quadra, Falcon | Works; slow, no colour. Needs A/UX 2.0.1 and a IIci ROM. |
| TOS (EmuTOS, TeraDesk) | `starttos` | Quadra, Falcon, Amiga | Works. fVDI at the screen's size; XaAES with `starttos -e xaaes`. On a Falcon, `starttos -P` runs the machine's own TOS ROM (slow redraws). |
| Amiga (Kickstart 3.2, Workbench) | `startmig` | Quadra, Amiga | Partial: Workbench on RTG (Picasso96 or ZZ9000), 8-, 16- and 32-bit where the host card has them; slow start; stock chipset display not supported. |
| Concurrent CP/M-68K | `startcpm` | Quadra, Falcon | Works, in the terminal: CP/M 3 BDOS, XDOS processes, more consoles with `startcpm -c N`, RSXes. Z80 and 8086 programs too. |
| Windows 3.x programs (Win16) | `startwin` | Quadra | Works for Program Manager and the tested programs. Each user installs their own Windows 3.1/3.11 and Wabi 2.2 with `startwin -install`. |
| X11R6.3 | `startx`, `xsession` | Quadra, Falcon | Works: twm, XView, xdm session chooser; sessions for each environment. OpenGL through Mesa 5 (software); `glxgears`. |

SMSQ/E, TI calculators, TT and more are planned (`docs/*-plan.md`).

## What you need

Nothing proprietary is in this repository and nothing proprietary is ever distributed.
You supply:

- the AMIX 2.1 tape archive (both parts) and its 2.1 patch disk
- Mac: Quadra 800 and Quadra 700 ROMs, the A/UX 3.1 CD, Mac OS 7.6.1 and 8.1 CDs; System 6 needs the A/UX 2.0.1 CD and a IIci ROM
- Falcon: TOS 4.04 for passthrough (EmuTOS is fetched)
- Amiga environment: the AmigaOS 3.2 CD and Picasso96; Amiga host: its Kickstart ROM and openpci.library
- CP/M: optionally Digital Research's CP/M-68K 1.3 release zip, for its programs (AS68, ED, STAT, ...); the system itself is ours
- Win16, at run time: Windows 3.1 or 3.11 and Wabi 2.2

`tools/setup.sh` builds the toolchains and fetches the open-source parts; see `BUILDING.md`.

## Build

```
sh kernel/build.sh                      # Quadra kernel: kernel/build/unix-mac.elf
GROUP=smoke sh tests/run-qemu.sh        # tests in QEMU q800; groups: smoke core mac tos cpm amiga display sound win16 gl stable
sh images/macenv/mkmacimage.sh          # Quadra disk image with every environment
PLATFORM=atari sh kernel/build.sh       # Falcon030 kernel (PLATFORM=atari060: CT60)
sh kernel/atari/mkdisk.sh               # Falcon disk image
PLATFORM=amiga sh kernel/build.sh       # Amiga kernel
sh kernel/amiga/test/mkhdf.sh           # Amiga disk
```

`BUILDING.md` lists the media each target needs; `sh tools/check-env.sh` reports what is
missing. [docs/hosts.md](docs/hosts.md) covers booting each host.

## Layout

| Path | Contents |
|---|---|
| `kernel/mac/` | Quadra 800 platform layer: boot, MMU, SCC, SCSI, console, ADB, SONIC, RTC, display and sound services |
| `kernel/atari/` | Falcon030 and CT60 platform layer, loader, disk image, Hatari tests |
| `kernel/amiga/` | Amiga host: amilib (AmigaOS libraries in the kernel), openpci, PCI and RTG drivers |
| `kernel/dlm/` | SVR4.2-style loadable kernel modules ([docs/modules.md](docs/modules.md)) |
| `kernel/guest/` | environments: A/UX personality and Mac (`mod/`), TOS (`tos/`), Amiga (`amiga/`), CP/M (`cpm/`), Win16 (`win16/`) |
| `kernel/otbridge/` | Open Transport bridge for Mac OS 8 networking |
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
- [EmuTOS](https://emutos.sourceforge.io/), fVDI, TeraDesk, XaAES: the TOS environment
- [FreeType](https://freetype.org/): TrueType for the Win16 environment (fetched at build time)
- [Mesa](https://mesa3d.org/) 5.0.2: OpenGL (fetched at build time)

## AI disclosure

Most of the code and documentation here was written with AI assistance (Claude, by
Anthropic), under the maintainer's direction. Every change is reviewed and run through
the test suite before it is merged; hardware results are reported separately from
emulator results.

## Licence

GPL-2.0 (see `LICENSE`). Files derived from other projects keep their own notices.
