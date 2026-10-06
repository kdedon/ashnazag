# X server for the display service

`Xamix`: the X11R6.3 AMIX port's server with an RTG card table, a card for display-service frame
buffers (`/dev/fbN`) and keyboard and mouse input from `/dev/kbd` and `/dev/mouse`. Cross-built
with the `m68k-cbm-sysv4` toolchain; clients are the stock AMIX X11R4 ones (tape segments 13,
14). Design: `docs/display-service-design.md` §3.

## Files

| Path | Content |
|---|---|
| `patches/01-rtg-card-table.diff` | port overlay: `rtgCardRec` table in `rtg.h`, `rtgInit.c` calls through it, ZZ9000 as one entry (`zz9000/zz9000card.c`, code moved from `rtgInit.c`), `-rtg name`, card options via `ddxProcessArgument`. No ZZ9000 behaviour change |
| `patches/02-fb-event-input.diff` | port overlay: `amixInput` hook (keyboard enable/disable, wakeup read), keymap pointers and key offset in `amixKbd.c`; `Xamix` target (`Xzz9000` is a link to it); `fb` directory |
| `patches/03-no-tcp.diff` | `os/access.c` includes `netinet/in.h` even without TCP (its interface code needs it) |
| `patches/05-iplan2p8.diff` | `iplan2p8` (interleaved 8 bitplanes, sources copied by `tree`) in `Xamix` |
| `patches/04-card-select.diff` | `RtgZZ9000` switch in the server, `rtg` and `amix` Imakefiles and `amixIo.c`: the ZZ9000 card is optional |
| `src/amixEv.c` | `inev` records → the port's `InputEvent` → `amixKbdEnqueueEvent`/`amixMouseEnqueueEvent` (autorepeat, acceleration unchanged); `-mb3` |
| `src/amixMacKeyMap.c` | ADB core keymap, keycode = ADB + 8; Command Mod1 (Meta), Option Mod2 (Mode_switch) |
| `src/rtg/fb/fbcard.c`, `Imakefile` | the `fb` card: session, `mmap`, mfb at 1 bpp (pixel 0 white), cfb at 8 bpp with colormap install/store → `FBIOPUTCMAP`, save screen → `FBIOBLANK` |
| `config/cross.def` | imake overrides: cross compiler, server only (no Xnest, Xprt, XKB, LBX, XIE, PEX), `UseRgbTxt`, `RtgZZ9000 NO`, `ConnectionFlags -DUNIXCONN` |
| `config/amixld.sh` | final link with the cross `ld` (crt files, shared libc, libm, libc's archive-only members) |
| `build.sh` | the build (below) |
| `package.sh` | `$X11W/pkg`: stripped server, BDF fonts with `fonts.dir`, `rgb.txt`, `startx`, `xinitrc`; writes `diskroot/x11-diskroot.diff` |
| `session/startx`, `session/xinitrc` | start `Xamix :0 -auth` with a fresh cookie, run the session script (xclock, xterm, `exec twm`), stop the server when it ends |
| `diskroot/x11-diskroot.diff` | `-p1` from the repository root: `mkdiskroot.sh` copies segments 13, 14; `root.manifest` adds them (minus the Amiga servers) and `/usr/x11r6` |
| `mkimage.sh` | disk image with X from a scratch copy of `kernel/mac/diskroot` |
| `runtest.sh` | QEMU run: serial login, `startx`, keys and mouse by QMP, hotkeys, `kill -9`, restart, twm Exit (`DEPTH=n` passes `-g 640x480xn`) |
| `results/` | QEMU screen dumps |

## Build

```
sh x11/build.sh            # stages: tree imake makefiles libs server
sh x11/package.sh
AMIX_TAPE=<tape dir> sh x11/mkimage.sh [kernel.elf [out.img]]   # ROOTMB 96
```

Work tree `$X11W` (default `images/work/x11-$PLATFORM`, PLATFORM mac or atari; the build needs a scratch directory). `tree` extracts `ref/x11r6.3/dist`
`xc-1..3`, the port overlay, all `patches/*.diff` and `src/`, plus `kernel/mac/display/dsio.h`. `imake`
builds a host `imake` (with `-DAMIX -DSVR4 -Dm68k`), uses host `cpp -undef` for it, and installs
`cross.def` into `site.def`'s after-vendor half; it also takes `amiga/screen.h` and
`libscreen.so` from tape segment 02 (the sysroot links them to absolute paths). Makefiles are
made only for `include`, `lib/{xtrans,Xau,Xdmcp,font}` and `programs/Xserver`. Everything runs
`nice -n 19`, `make -j1`.

Toolchain notes:
- `m68k-cbm-sysv4-ar` aborts ("buffer overflow detected"); archives use `m68k-elf-ar`.
- `libsocket.so` references `syslog` and `seteuid`, which only the static libc has; GNU ld 2.8.1
  refuses the link. `amixld.sh` accepts exactly those two (lazy binding, never called) and
  fails on anything else.
- `RtgZZ9000` (default YES) selects the ZZ9000 card; `cross.def` sets NO, so the Mac server links only the `fb` card, without `-DZZ9000` and the `-mode` option. `/usr/bin/startx` links to `/usr/x11r6/bin/startx`.
- Shared objects are linked by `-L`/`-l`, so `DT_NEEDED` is `libsocket.so`, `libnsl.so`,
  `/usr/amiga/lib/libscreen.so`, as native clients have.

Build time: libraries ~1 min, server (261 files) ~1–2 min.

## Run

```
Xamix :0 [-fb /dev/fb0] [-mb3] [-rtg fb|zz9000] [-fp path] [-depth n] [-kbd us|de]
/usr/x11r6/bin/startx [script] [-- server options]      # log: /tmp/Xamix.log
```

- Cards are probed in order zz9000, fb; `-rtg` picks one.
- The session is acquired with `FBA_FRONT`; on `EPERM` (not root, console in front) it is
  acquired in the background and the log says so; Control-Option-Command + digit brings it front.
- The session and input fds live for the whole process: a server reset keeps them; exit or
  `kill -9` releases them and the kernel brings back the console.
- Hidden/shown notes on the fb fd only reset `-mb3` and Option state: the kernel restores VRAM
  and the colour table, and sends key ups at switch-out.
- Event times: the kernel stamps records with the tick fraction, so they can lead
  `gettimeofday()` by a few ms. Times ahead of `gettimeofday()` or over 1 s old are replaced by
  the read time (otherwise the port's autorepeat fires at once).
- Fonts: MIT BDF (misc subset, 75dpi Helvetica 8–14), read directly (no PCF compiler).

## Security

- No TCP: R6.3 has no `-nolisten`, so `ConnectionFlags` is `-DUNIXCONN` only. Display `127.0.0.1:0` and `q800:0` fail to open.
- `startx` writes a fresh MIT-MAGIC-COOKIE-1 for `unix:0` and `:0` into `$HOME/.Xauthority` (mode 600, stock AMIX `xauth`; `$HOME` is `/` for root) and starts `Xamix -auth`. The stock AMIX clients (R5-level Xlib) read `XAUTHORITY`/`.Xauthority`; with `XAUTHORITY=/nonexist` the server refuses ("Client is not authorized to connect").
- AMIX has no `/dev/urandom`: each 16-bit cookie word is a `sum` of the time, pid, process table and file listing. It varies little, so the cookie is weak against a local attacker who can observe the boot time; use a kernel random device when there is one.

## Tests

QEMU q800, Quadra 800 ROM, 64 MB, kernel `kernel/build/unix-mac.elf`, disk from `mkimage.sh`,
`runtest.sh`. The ROM leaves DAFB at **640×480×8** (row 1024); `-g 640x480x1` does not change it,
so the server runs the **8 bpp cfb path** (PseudoColor, default map loaded through
`FBIOPUTCMAP`), not mfb. Screens in `results/8bpp/`:

| Check | Result |
|---|---|
| `startx` as root | twm (colour title bars), xterm, xclock (`x1`) |
| typing in xterm (ADB → `/dev/kbd` → X) | `xdpyinfo`: depth 1 and 8 formats, keycodes 8–135, 10 extensions (`x2`); shifted characters right |
| server log in xterm | only `fb` card lines (no probe of another card), `/dev/fb0 DAFB 640x480 depth 8 rowbytes 1024`, no time warnings (`x3-log`) |
| Control-Option-Command-0 / -1 | console with its text; X back intact (`console`, `xback`) |
| mouse | pointer moves, twm root menu on button 1 (`menu`) |
| twm Exit | session ends, server stopped, console in front (`twmexit`, `twmexit-after-restart`) |
| `kill -9` of the server | console in front, clients get broken pipe (`kill9`) |
| second `startx` | runs again (`restart`) |

## Known limits

- 1 bpp (mfb, pixel 0 white) is built but not run: needs a 1-bpp boot (A/UX Startup, or PRAM
  depth) or `FBIOSMODE`. 16/32 bpp not done; `xcmap`-style colormap tests not run.
- `-mb3`, Control/Option/Command modifiers in clients not exercised by the test.
- Serial console input can stop mid-line while X is in front; the test drives everything after `startx` by keyboard.
- `EVIOCBELL` does not exist yet: no bell. LEDs not driven.
- `EVIOCGKEYS` resync on `FBN_SHOWN` not done.
- XKB off (`BuildXKB NO`); core keymap only, US layout.
- Native rebuild in QEMU not done.

## Packages: manx, XView

```
sh x11/build.sh clibs              # libX11.a, libXext.a, Xt headers
sh x11/manx/build.sh               # $X11W/manx/MANX.pkg   (/opt/amix/bin)
sh x11/xview/build.sh              # $X11W/xview/XVIEW.pkg (/usr/openwin)
XPKGS="manx xview" AMIX_TAPE=... sh x11/mkimage.sh   # installs them, as pkgadd would
```

Sources come from `images/vendor.manifest` through `images/fetch.sh` (pinned sha256 or commit,
cached in `toolchain/dl/vendor`). Streams are SVR4 datastreams (`mkpkg.py`), so `pkgadd -d` takes
them too. Build times: clibs ~20 s, manx ~30 s, XView ~45 s.

- manx (C99): m68k Linux gcc 13 to assembly with `-malign-int`, its assembler, AMIX `ld`; static,
  with R6.3 `libX11.a`. `manx` 344 KB, `xmanx` 764 KB, `manxtrust` 227 KB (stripped).
- XView: upstream tarball, the sp1 patch, then `xview/xview-amix.diff` (AMIX `sigaction` and
  `sigset_t` layouts). `mktc.sh` makes a toolchain copy whose gcc sets a0 = d0 at every return
  (XView calls `Xv_opaque` functions through pointer prototypes) and whose headers give the
  `-Xa` namespace with `__STDC__` = 1. Static X and XView libraries, shared libc; `compat.c` has
  the BSD calls libc lacks (`index`, `rindex`, `bcopy`, `bzero`, `random`, `srandom`, `getwd`,
  `killpg`). Clients: olwm, olvwm, olwmslave, cmdtool, textedit, clock, props (9 MB).
- Both use the R6.3 libraries, which speak only the UNIX-domain transport (`ConnectionFlags`).

## xdm and sessions

```
sh x11/build.sh clibs clients      # ICE SM Xt Xmu Xaw, xdm, xchoose, xdmenv
BOOTX=1 sh x11/mkimage.sh ...      # boot to xdm; /etc/default/x BOOT=xdm|console
sh x11/xdmtest.sh OUTDIR [image]   # QEMU: login, chooser, twm, XView, Console, TOS, Mac
```

init's `co` entry runs `xdmboot`: xdm (`-nodaemon`, no XDMCP port, server with `-auth`) when
`/etc/default/x` says `BOOT=xdm`, else getty; a text login also follows xdm's exit. After login
`xsession` offers twm, XView, the installed environments and a full-screen Console xterm,
remembering the choice in `~/.xsession-choice`. `xsession NAME` from a text console starts X
with that session. `xdmenv` (setuid root) starts an environment for the user xdm logged in on
the screen, with the console as its terminal, and brings X back afterwards.
`patches/06-xdm.diff`: accounts without a password log in with an empty one on the local
screen (root too, if it has none), unless `/etc/default/login` has `PASSREQ=YES`; the session
takes a user licence before `setuid` (non-root only); the cookie comes from an XTEA-hashed pool
(clock, ticks, process times, `/dev` entry times, a seed kept in `xdm-seed`, mode 600).
