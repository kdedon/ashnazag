# Environments

Each environment is a Unix process with its own display session, run by an ordinary user
in group `display`. Ctrl-Option-Command-0 (Falcon: Ctrl-Alt-0) shows the console, -1 to -9
the sessions; Ctrl-C on the console ends TOS or the Amiga. A user's own copy of each
system lives in the home directory; `makemac`, `maketos` and `makeamiga` make it, `-f`
refreshes it from the system's copy keeping the user's files, `-e NAME` makes a separate
named environment. One session runs per copy.

Operating-system media are yours to supply; the image builders install them from your
CDs and ROMs (`BUILDING.md`).

## Mac

| Command | System | Leave with |
|---|---|---|
| `startmac` | 7.6.1 | Special > Log Out (root: Apple menu) |
| `makemac -e 8.1`, then `startmac -e 8.1` | 8.1 | Special > Log Out (root: Apple menu) |
| `startmac6` | 6.0.7 with MultiFinder | Special > Logout |

A/UX 3.1's Mac environment (A/UX 2.0.1's for System 6). Root's System Folder is
`/mac/sys/System Folder`, other users' `~/System Folder`. Root's Shut Down halts Unix.
`/etc/aux/macenv.conf` sets `TBMEMORY` (default a quarter of RAM, 8-32 MB) and, for root,
`MACMODEL`.

Known issues: System 6 is slow and has no colour.

## TOS

`starttos` boots EmuTOS with the TeraDesk desktop and fVDI at the screen's size.

- C: `~/TOS` (else `/tos/sys`, read-only), G: games, U: `/`; more in `~/TOS/drives` or `-D L=dir`.
- `-e xaaes`: XaAES multitasking AES. `maketos -x -e NAME` makes more.
- `-M` ST high monochrome, `-S` the ST screen, `-m MB` ST-RAM.
- `-C DIR` boots a program folder: `starttos -C ~/TOS/PROGRAM`.
- `-rom FILE`: your own TOS ROM. On a Falcon, `-P` runs the machine's own TOS on its
  hardware.

## Amiga

`startmig` boots Kickstart 3.2 to Workbench; SYS: is `~/Amiga` (`makeamiga`). `-m MB` sets
fast RAM (or `fastmb=MB` in `.env`). The display is RTG through Picasso96 (`container.card`,
`kernel/guest/amiga/rtg`); there is no chipset display. Sound goes through AHI. Warp3D.library draws with software Mesa; hardware 3D cards come later.

## CP/M-68K

`startcpm` runs Concurrent CP/M-68K in the terminal. Ctrl-C stops the program inside; `EXIT`
(or closing the terminal) ends the session. `startcpm -c 1` (to 3) in
another terminal joins it as a further console. Drives are directories:
A: `~/CPM` (user areas 1-15 are subdirectories), H: home, M: a private scratch directory,
O: `/`, P: Digital Research's programs. Z80 and 8086 CP/M programs run too.

## Win16

`startwin` runs Windows 3.x programs with its own KERNEL, USER and GDI and an x86
interpreter, as Wabi did. Install once from your own Windows 3.1/3.11 disks and Wabi 2.2:

```sh
startwin -install DISK1.IMG ... DISK6.IMG /cdrom/Wabi
```

Then `startwin` opens Program Manager, `startwin notepad.exe` one program. C: is `~/WIN16`,
H: home, R: `/`. Design: `docs/win16-design.md`.

## X11

`startx` starts twm; `xsession` offers a menu, or `xsession twm|xview|mac|tos|amiga|win|smsqe|lisa|voyage200|console|cpm`.
With xdm the session chooser lists the same.

## Native SMSQ/E, Lisa and TI Voyage200 ports

`startsmsqe`, `startlisa` and `startvoyage200` launch caller-supplied AMIX m68k ELF executables as
separate Unix processes. Install the launchers with
`sh kernel/guest/nativeenv/build.sh OUTDIR DESTDIR`.

Each family has its own directory: `~/SMSQE`, `~/Lisa` or `~/Voyage200`; `-e NAME` selects a
subdirectory. Create that directory, install your executable as `native` and
provide a readable file or directory as `media`. Explicit absolute paths use
`-x /path/to/executable -m /path/to/media`. Arguments after `--` pass through.
For example:

```sh
mkdir -p "$HOME/SMSQE"
startsmsqe -x /opt/native/smsqe -m /home/guest/disks/smsqe -- application
```

The executable inherits terminal input/output, the environment directory as its
working directory, and its `.env` session lock. It receives
`NATIVEHOST_ENVIRONMENT`, `NATIVEHOST_ROOT`, and `NATIVEHOST_MEDIA`. Its native
port defines media interpretation and uses `nativehost` for AMIX services and
display ownership. The launcher preserves the executable's process exit status
and signal handling through `exec`.

`/etc/default/smsqe`, `/etc/default/lisa` and `/etc/default/voyage200` use the existing `LEGACY_ROOT`,
`ENVIRONMENT_ROOT` and `ENVIRONMENT_METADATA` policy keys. `-q` checks payload
availability; the X chooser lists each family only when its default executable
and media are available. Missing or incompatible executables and missing media
fail before launching. OS payloads and native ports must be supplied separately.
