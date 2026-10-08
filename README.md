# Ash Nazag - The One Distribution

One SVR4 kernel and userland for 68k Macintosh, Amiga and Atari machines, built on
Commodore's AMIX 2.1 and the community AMIX 68040/68060 port, with A/UX's Macintosh
environment running on top.

Status: a Quadra 800 boots this kernel straight from disk to a multi-user login on its
built-in screen, with SCSI, Ethernet, a clock, loadable kernel modules and support for
A/UX programs (static and shared-library, e.g. `sh`, `ls`, `more`, `vi`). A/UX's Mac
environment runs in its own screen session up to an idle Finder desktop, with mouse, keyboard
and Ctrl-Opt-Cmd-digit switching between the Mac and the console. Tested in
QEMU's `q800` machine with a real Quadra 800 ROM; hardware testing is in progress.

## Layout

| Path | Contents |
|---|---|
| `kernel/mac/` | Quadra 800 platform layer: boot, MMU map, SCC, 53C96 SCSI, DAFB console, ADB, SONIC, RTC, device tables, root selection, boot blocks |
| `kernel/dlm/` | SVR4.2-style loadable kernel modules |
| `kernel/amiga/` | Amiga host: amilib (AmigaOS libraries run in the kernel) and the opci module, the PCI bus through openpci.library |
| `kernel/guest/` | A/UX personality: trap gates, A/UX system calls, COFF and shared-library exec, Mac environment interface (screens, events, cursor, PRAM); the TOS (`tos/`), CP/M-68K (`cpm/`) and Windows 3.x (`win16/`, see `docs/win16-design.md`) environments |
| `kernel/otbridge/` | Open Transport bridge: STREAMS module, virtual Ethernet stations on the SONIC, ASLM library builder, Mac `.ENET` driver |
| `x11/` | X11R6.3 server for the Mac screen (build, patches, session scripts, image builder) |
| `kernel/tools/`, `kernel/build.sh` | build: AMIX link kit relink, port build, Mac overlay, checks |
| `kernel/port-local.diff` | three local changes to the 040/060 port |
| `images/` | disk-image builders (A/UX Startup, direct boot, disk root) and QEMU scripts |
| `tests/` | functional test suite run under QEMU |
| `tools/` | COFF/ELF converter and binary tools |
| `docs/` | design documents |

## What you need

Nothing proprietary is in this repository. To build you supply:

- the AMIX 2.1 tape archive (both parts) and its 2.1 patch disk
- for disk images and the Mac environment: A/UX 3.1, Mac OS CDs and Quadra ROM images
- for the Windows 3.x environment, at run time (each user installs it with `startwin -install`): Windows 3.1 or 3.11 and, for Wabi's tools and printer drivers, Wabi 2.2

`tools/setup.sh` builds the toolchains and fetches the rest; see `BUILDING.md`.

## Built on

- [amix-040-060-port](https://github.com/asokero/amix-040-060-port): 68040/68060 support for AMIX
- [gcc-cross-amix](https://github.com/isoriano1968/gcc-cross-amix): AMIX cross toolchain
- [x11r6.3-amix](https://github.com/isoriano1968/x11r6.3-amix): X11R6.3 for AMIX, base of our Mac server
- [amigaux.org](https://amigaux.org/): AMIX packages

## Build

```
sh kernel/build.sh          # kernel/build/unix-mac.elf, unix-mac.coff
sh images/mkboot.sh         # images/q800-unix.img (direct boot, RAM root)
sh kernel/mac/diskroot/mkdiskimage.sh   # disk-root image
sh tests/run-qemu.sh        # test suite in QEMU q800
```

`BUILDING.md` lists the media each target needs; `sh tools/check-env.sh` reports what is
missing.

## AI disclosure

Most of the code and documentation here was written with AI assistance (Claude, by
Anthropic), under the maintainer's direction. Every change is reviewed and run through
the test suite before it is merged; hardware results are reported separately from
emulator results.

## Licence

GPL-2.0 (see `LICENSE`). Files derived from other projects keep their own notices.
