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
| `kernel/guest/` | A/UX personality: trap gates, A/UX system calls, COFF and shared-library exec, Mac environment interface (screens, events, cursor, PRAM) |
| `kernel/tools/`, `kernel/build.sh` | build: AMIX link kit relink, port build, Mac overlay, checks |
| `kernel/port-local.diff` | two local changes to the 040/060 port |
| `images/` | disk-image builders (A/UX Startup, direct boot, disk root) and QEMU scripts |
| `tests/` | functional test suite run under QEMU |
| `tools/` | COFF/ELF converter and binary tools |
| `docs/` | design documents |

## What you need

Nothing proprietary is in this repository. To build you supply:

- AMIX 2.1 (tape or installation) and its 2.1 patch disk
- for the Mac environment: A/UX 3.1 and a Quadra ROM image
- the toolchains below

## Built on

- [amix-040-060-port](https://github.com/asokero/amix-040-060-port): 68040/68060 support for AMIX
- [gcc-cross-amix](https://github.com/isoriano1968/gcc-cross-amix): AMIX cross toolchain
- [x11r6.3-amix](https://github.com/isoriano1968/x11r6.3-amix): X11R6.3 for AMIX (planned Mac backend)
- [amigaux.org](https://amigaux.org/): AMIX packages

## Build

```
sh kernel/build.sh          # kernel/build/unix-mac.elf, unix-mac.coff
sh images/mkboot.sh         # images/q800-unix.img (direct boot, RAM root)
sh kernel/mac/diskroot/mkdiskimage.sh   # disk-root image
sh tests/run-qemu.sh        # test suite in QEMU q800
```

Paths to your AMIX, A/UX and ROM files are set in the scripts; see `kernel/README.md`
and `images/README.md`.

## AI disclosure

Most of the code and documentation here was written with AI assistance (Claude, by
Anthropic), under the maintainer's direction. Every change is reviewed and run through
the test suite before it is merged; hardware results are reported separately from
emulator results.

## Licence

GPL-2.0 (see `LICENSE`). Files derived from other projects keep their own notices.
