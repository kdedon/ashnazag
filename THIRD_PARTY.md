# Third-party components

The repository is MIT (`LICENSE`). Components below keep their own licences and notices. Where a source file carries a notice, it stays in the file.

## Permissive components

| Component | Where | Licence and notice |
|---|---|---|
| XFree86 4.3 DRM | `kernel/drm` | MIT/X11. Copyright Precision Insight, VA Linux Systems, Tungsten Graphics, The Weather Channel. Built as loadable modules only |
| XFree86 / Mesa / Glide3 patches | `gl/`, `x11/` patches | Patches against MIT/X11 (XFree86 3.3.6 and 4.3 DRI, X11R6.3, Mesa 5 MIT) code, fetched at build. Our new files: MIT. Glide3: see below |
| asokero `amix-040-060-port` | `kernel/port-*.diff`, `kernel/port-tree.sh`, derived `pstart*.s` | MIT, Copyright asokero. Derived files say so |
| hmlio `amix-gayle-ide` | `kernel/amiga/ide` | MIT, Copyright (c) 2026 Emre Bastuz (`kernel/amiga/ide/LICENSE`) |
| jusii `amix-cdfs` | fetched at a pinned commit; glue `kernel/fs/cdfs` | BSD 2-clause, Copyright (c) 2026 Stefan Reinauer, Jussi Alanärä. Built as a loadable module only |
| ZZ9000 driver | `kernel/amiga/zz9000` | MIT, Copyright (c) 2026 Ignacio Soriano (`LICENSE`) |
| Radeon init tables | `kernel/amiga/radeon/rdsinit.c` | MIT. Copyright 2004 ATI Technologies, 2007-8 Advanced Micro Devices, 2008 Red Hat. Notice in the file |
| NetBSD m68k FPE | fetched at build from a pinned NetBSD tarball; glue `fpe_glue.c`, `fpe040.s` | BSD, see below |
| Motorola M68040/M68060 software packages (FPSP, ISP) | fetched at build from the pinned NetBSD tarball; glue in the port and `kernel/port-isp060.diff` | Copyright Motorola Inc., see below |
| NetBSD `ncr53cxxx` SCRIPTS assembler | output of `kernel/amiga/ncr770/script.sh` | BSD (NetBSD notice kept with the generated source) |
| Sony font | `kernel/mac/video/fbfont.c`, `mkfont.py` | HPND: Copyright (c) 1987, 1988 Sony Corp. Permission notice kept in the file |
| Adobe / DEC 100dpi fonts | `kernel/guest/win16/fonts.c`, `mkfonts.py` | Copyright 1984-1989, 1994 Adobe Systems Incorporated; 1988, 1994 Digital Equipment Corporation. Permission notice in `NOTES.md` |
| P96 SDK board offsets | `kernel/guest/amiga/rtg/p96offsets.inc` | CC-BY-4.0, attribution in `NOTICE` |
| Wine 16-bit `.spec` facts | `kernel/guest/win16/apitab.c`, `mkapi.py` | Wine is LGPL-2.1. Only function names and ordinals are used, no spec text |
| sdhcp | `images/envpkg/sdhcp` (from github.com/michaelforney/sdhcp at 5f9956c) | MIT, Copyright 2012 David Galos, 2014-2015 Hiltjo Posthuma, 2015 Michael Forney (`LICENSE`, installed with the package) |
| top 3.5.1 | `images/envpkg/top` (from unixtop 3.5.1) | Freely redistributable with its notice intact, Copyright 1984-2004 William LeFebvre (`LICENSE`, installed with the package). Machine module and changes: MIT |
| Dropbear, prngd, libamixcompat | `images/pkgs/dropbear`, `prngd` patches; `images/pkgs/port` builds jusii/amix-packages libamixcompat and passwd parsers | Fetched at build. Dropbear MIT (Matt Johnston and others); prngd free licence, Lutz Jaenicke; libamixcompat BSD-2 and amix-pwgr.c MIT, Jussi Alanärä |
| Vim 9.2 | `images/ports/vim`, `vimdoc`, fetched at build; configure answers, vimrc settings and the `%ll` formatting after slaapliedje/atari-sysv-sp1 `amix/vim` | Vim licence (charityware, GPL-compatible), Bram Moolenaar and others (`LICENSE`, installed with the package) |
| go-lha | `webapp/internal/lhadecode` | MIT, Copyright (c) 2018 MURAOKA Taro (`LICENSE`, `NOTICE`) |
| SMSQ/E | fetched | BSD 2-clause |
| Digital Research CP/M-68K 1.3 CCP | `kernel/guest/cpm/ccp/ccp.c`, `ccpdef.h` | Copyright (c) 1982 Digital Research. Distributed under the Caldera 2001 licence and DR DOS, Inc.'s 2022 statement permitting use and redistribution with the notice kept. The file headers say "Portions Copyright (c) 2026 kdedon": only the additions are ours (MIT). `ccpext.c`, `ccpgo.c`, `ccpsv.h` and `cmd/*` are ours (MIT) |

### NetBSD FPE (`sys/arch/m68k/fpe`, checked in the pinned tarball)

Copyright holders: The Regents of the University of California (Lawrence Berkeley Laboratory, 1992, 1993); Gordon W. Ross (1995); Ken Nakata (1995); Tetsuya Isaki (2011, 2013).

- 12 files carry a BSD 4-clause licence (`fpu_add.c`, `fpu_div.c`, `fpu_mul.c`, `fpu_sqrt.c`, `fpu_subr.c`, `fpu_explode.c`, `fpu_implode.c`, `fpu_fscale.c`, `fpu_calcea.c`, `fpu_emulate.c`, `fpu_emulate.h`, `fpu_arith.h`). The advertising clause applies:
  - "This product includes software developed by the University of California, Lawrence Berkeley Laboratory."
  - "This product includes software developed by Gordon Ross."
- The rest are BSD 3-clause or 2-clause. Each file's notice stays with the source.
- Binary releases of the Falcon030 kernel, whose image contains this code, must carry the notices above in their documentation. Advertising material mentioning features of the kernel must carry them too.

### Motorola M68060 Software Package (`sys/arch/m68k/060sp`, checked in the pinned tarball)

The ISP is linked into the kernel modified: its call-outs are ours (`isp060_head.s`, `isp060_glue.s`). Its notice, verbatim:

```
MOTOROLA MICROPROCESSOR & MEMORY TECHNOLOGY GROUP
M68000 Hi-Performance Microprocessor Division
M68060 Software Package Production Release 

M68060 Software Package Copyright (C) 1993, 1994, 1995, 1996 Motorola Inc.
All rights reserved.

THE SOFTWARE is provided on an "AS IS" basis and without warranty.
To the maximum extent permitted by applicable law,
MOTOROLA DISCLAIMS ALL WARRANTIES WHETHER EXPRESS OR IMPLIED,
INCLUDING IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS
FOR A PARTICULAR PURPOSE and any warranty against infringement with
regard to the SOFTWARE (INCLUDING ANY MODIFIED VERSIONS THEREOF)
and any accompanying written materials. 

To the maximum extent permitted by applicable law,
IN NO EVENT SHALL MOTOROLA BE LIABLE FOR ANY DAMAGES WHATSOEVER
(INCLUDING WITHOUT LIMITATION, DAMAGES FOR LOSS OF BUSINESS PROFITS,
BUSINESS INTERRUPTION, LOSS OF BUSINESS INFORMATION, OR OTHER PECUNIARY LOSS)
ARISING OF THE USE OR INABILITY TO USE THE SOFTWARE.

Motorola assumes no responsibility for the maintenance and support
of the SOFTWARE.  

You are hereby granted a copyright license to use, modify, and distribute the
SOFTWARE so long as this entire notice is retained without alteration
in any modified and/or redistributed versions, and that such modified
versions are clearly identified as such.
No licenses are granted by implication, estoppel or otherwise under any
patents or trademarks of Motorola, Inc.
```

### Glide3

`gl/` fetches the 3dfx Glide 2002.04.10 sources and patches them. Their licence is the **3dfx GLIDE Source Code General Public License**, not a BSD-style one. It is copyleft limited to the Glide portion: the identifiable part of a derivative work that performs 3D graphics API operations, which must be released as source under the same licence. Trademark use is excluded. The patches and the Glide library stay under it. It is built as a separate user-space library, never part of a kernel image.

## Components that stay GPL

These are separate programs, patches or separately loaded modules and keep their licence and headers. Nothing GPL is statically linked into a kernel image: GPL code ships only as separate programs or separately loaded modules.

| Component | Form | Licence |
|---|---|---|
| Hatari (`kernel/atari/hatari-*.patch`, `ct60/svmodel.diff`) | patches to the emulator | GPL-2.0-or-later |
| QEMU (`toolchain/qemu-patches`, `kernel/cow/*.diff`) | patches | GPL-2.0 |
| hfsutils (`toolchain/hfsutils-patches`) | patches | GPL-2.0 |
| GNU wget 1.11.4, Lynx 2.8.5 (`images/pkgs/wget`, `lynx`) | patches; separate programs in their packages | GPL-3.0-or-later, GPL-2.0 |
| Quake (`gl/quake`, in `asokero/quake-amix`) | separate game program | GPL-2.0 |
| fVDI (`kernel/guest/tos/fvdi`, `ashfb.c` built with it) | TOS-side program, separate image | GPL |
| XaAES, TeraDesk, FreeMiNT | fetched at build, source shipped with the binaries | GPL |
| EmuTOS | fetched, separate image, patched in memory only | GPL-2.0-or-later |
| GNU bash 5.2.37 (`images/ports/bash`; `bash.diff` and `hostfix.h` from slaapliedje/atari-sysv-sp1 `amix/bash`) | patch; separate program in its package | GPL-3.0-or-later |
| manx, atari-sysv-sp1 (slaapliedje) | fetched at build | GPL-2.0 |

Files that are ours inside these projects' patches follow the patched project's licence.
