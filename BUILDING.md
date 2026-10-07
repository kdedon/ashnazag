# Building

Nothing proprietary is in this repository. You supply your own media; `tools/setup.sh`
fetches and builds everything else into the checkout (`toolchain/`, `kernel/`, `ref/`,
`media/`), never outside it.

## Quick start

```sh
sh tools/setup.sh --tape amix_2.1_tape_part1.tar.bz2 amix_2.1_tape_part2.tar.bz2 \
    --patch amix_21_patch.adf --qemu
sh tools/check-env.sh suite
sh kernel/build.sh
sh tests/run-qemu.sh          # direct-boot test suite, results in tests/results/
```

Setup is safe to rerun and skips finished steps. Rerun it after moving the checkout:
two generated files hold absolute paths. The path must not contain spaces.

## Host

Ubuntu 24.04 or similar, x86-64:

```sh
sudo apt install build-essential bison flex m4 texinfo git curl cpio python3 \
    libgmp-dev libmpfr-dev libmpc-dev p7zip-full unzip \
    libglib2.0-dev libpixman-1-dev pkg-config python3-venv
```

`lhasa` works in place of `p7zip-full` for the patch disk; 7z is still needed for `.7z` CD
images. The last four packages are only for building QEMU.

## Toolchains (built by setup.sh)

| Where | What | Source |
|---|---|---|
| `toolchain/amix` | AMIX cross compiler `m68k-cbm-sysv4`: gcc 2.7.2.3, binutils 2.8.1 | [gcc-cross-amix](https://github.com/isoriano1968/gcc-cross-amix) at `206bc95`, `make all PREFIX=toolchain/amix AMIX_ROOT=toolchain/amix-root` |
| `toolchain/src/gcc-cross-amix/build/env.sh` | its environment, sourced by the kernel scripts | written by `make env`; setup rewrites it on every run |
| `toolchain/amix-root` | AMIX headers, libraries and link kit | your tape, segments 02 (core), 04 (Cdev), 19 (conf) |
| `toolchain/bin` | `m68k-elf` binutils 2.43.1 | GNU, `--target=m68k-elf --disable-nls --disable-werror` |
| `toolchain/linux` | `m68k-linux-gnu` binutils 2.43.1 and gcc 13.3.0 (C only, no libc) | GNU; gcc with `--without-headers --disable-shared --disable-threads --disable-multilib`, `all-gcc all-target-libgcc` |
| `toolchain/bin/h*` | hfsutils 3.2.6 with hfsck | Debian's `hfsutils_3.2.6.orig.tar.gz` plus `toolchain/hfsutils-patches` |
| `toolchain/dl/syssrc.tgz` | NetBSD 10.1 kernel sources (68040/060 support packages) | archive.netbsd.org, sha256 pinned |
| `toolchain/qemu-local` | QEMU 8.2.2 with `toolchain/qemu-patches` | `setup.sh --qemu` (runs `toolchain/build-qemu.sh`) |
| `kernel/amix-040-060-port` | 68040/68060 port | [amix-040-060-port](https://github.com/asokero/amix-040-060-port) at `54fba4d` plus `kernel/port-local.diff`; setup writes its `config.sh` |

The patched QEMU is required for the tests: stock QEMU 8.2 loses 68040 page writes under
paging. Ubuntu 24.04's `gcc-m68k-linux-gnu` and `binutils-m68k-linux-gnu` (gcc 13.2,
binutils 2.42) build the Atari loader in CI; installed normally, distribution packages
work. Unpacked with `dpkg-deb -x`, binutils' own libraries need
`patchelf --set-rpath '$ORIGIN/../lib/x86_64-linux-gnu'` on its binaries. Build times on a slow 4-core machine at `-j1`: gcc-cross-amix about
5 minutes, each binutils about 10, gcc 13.3.0 about an hour, QEMU about 40 minutes;
`JOBS=4` shortens them. `--amix-cross DIR` copies an existing gcc-cross-amix install
instead of building it.

## Your media

| Option | File | Used for |
|---|---|---|
| `--tape FILE...` | the AMIX 2.1 tape archive (both parts); also a SIMH `.tap`, a raw image or a directory of segments | kernel (02, 04, 19), RAM-disk root (02), disk root (02, 03, 07, 10), X (13, 14) |
| `--patch FILE` | AMIX 2.1 patch disk ADF | kernel (2.1c sources `c0.c`, `aen.c`, `kernel.c`) |
| `--q800-rom FILE` | Quadra 800 ROM | ROM boot in QEMU, Mac environment desktop |
| `--q700-rom FILE` | Quadra 700 ROM `420DBFF3` | A/UX guest tests, Mac environment on hosts without a ROM |
| `--aux-cd FILE` | A/UX 3.1 CD | Apple driver and map for ROM-bootable disks, `images/mkimage.sh`, the A/UX root tree |
| `--macos761 FILE` | Mac OS 7.6.1 CD (`.iso` or `.7z`) | Mac environment, Mac guest tests |
| `--macos81 FILE` | Mac OS 8.1 CD | Mac OS 8 guest tests, Open Transport bridge tests |
| `--tos FILE` | TOS 3.06 ROM zip | optional TOS test (EmuTOS is the default) |
| `--amiga-cd FILE` | AmigaOS 3.2 CD | Amiga environment (Kickstart from the CD) |

Pass your tape image or archive as it is; setup finds segments `00`…`28` by size and
sha256, writes them to `media/amix-tape` and names any that are missing. The kernel needs
02, 04 and 19; the first archive part alone (00–19) is enough for the kernel and the suite.

Optional open-source downloads: `--tos-src` (EmuTOS 1.4, fVDI), `--x11` (X11R6.3, x11r6.3-amix).

### What each target needs

`sh tools/check-env.sh TARGET` lists what is missing.

| Target | Command | Needs |
|---|---|---|
| kernel | `sh kernel/build.sh` | tape (02, 04, 19), patch disk, toolchains |
| suite (minimum) | `sh tests/run-qemu.sh` | kernel + patched QEMU. No ROM: QEMU boots the kernel directly. Guest tests skip without their media. |
| disk | `sh images/mkimage.sh --small`, `images/mkboot.sh`, `kernel/mac/diskroot/mkdiskimage.sh` | suite + A/UX 3.1 CD, hfsutils, tape 03, 07 and 10; Quadra 800 ROM to boot it |
| x11 | `x11/build.sh`, `x11/mkimage.sh` | disk + tape 13, 14, `--x11` |
| mac | `images/macenv/mkmacimage.sh` | x11 + Mac OS 7.6.1 CD, Quadra 700 ROM, the A/UX root tree (`tests/aux/auxroot`, from `--aux-cd`) |
| tos | `tests/run-qemu.sh` (t_tos) | suite + `--tos-src` |
| amiga | `tests/run-qemu.sh` (t_amiga), `images/amigaenv/mkamiga.sh` | suite + AmigaOS 3.2 CD |
| falcon | `PLATFORM=atari sh kernel/build.sh`, `kernel/atari/mkdisk.sh` | kernel + tape 02, 03, 10, `--tos-src`; Hatari for tests (`kernel/atari/build-hatari.sh`) |

## The A/UX disk

Disks the Quadra ROM boots need an Apple partition map and Apple's SCSI driver. The
direct-boot path (`tests/run-qemu.sh`, `images/qemu/run-direct.sh`) needs neither.

`images/mkboot.sh` takes the partition map layout and Apple's driver from the A/UX 3.1 CD
(`--aux-cd`) at build time; nothing of Apple's is stored in the repository. Any disk with one
`Apple_Driver` partition at block 64 and one HFS partition also works (`BOOT_SOURCE=disk.img`).

`images/mkimage.sh` (A/UX Startup boot) writes the CD as a disk and adds our kernel to its
HFS partition.

`--aux-cd` also extracts the A/UX root (`images/mkauxroot.sh`, from the CD's `UNIX Root&Usr`
partition) into `images/work/auxroot-cd`, the tree `tests/aux/auxroot` names. Device nodes
become `NAME.__special__` files; `auxroot-cd.meta` lists every entry's mode, owner and time.

A/UX 2.x media will be needed later for a System 6 environment on smaller machines.
