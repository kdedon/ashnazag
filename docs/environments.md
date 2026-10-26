# Environments

Each environment is a Unix process with its own display session, run by an ordinary user
in group `display`. Ctrl-Option-Command-0 (Falcon: Ctrl-Alt-0) shows the console, -1 to -9
the sessions; Ctrl-C on the console ends TOS or the Amiga.

Each family has one start command. `-e NAME` runs environment NAME; its first start makes
your own copy in your home directory. A new NAME is your private copy of `-b BASE`, by
default the default, e.g. `startmac -e work -b 8.1`. `-R` (Mac, TOS, Amiga) copies the
template over your copy again, keeping your other files. `-d NAME` sets your default
(with `-e` or `-C`, then runs); without `-e` the default runs, else the system's
(`DEFAULT_ENVIRONMENT` in `/etc/default/<family>`). `-l` lists the environments and the
default. One session runs per copy.

Operating-system media are yours to supply; the image builders install them from your
CDs and ROMs (`BUILDING.md`).

Environments start at the host's screen size. The display service also lists the host's
modes and grants a session's request for one (`fbmode` on Amiga images lists and tries
them); a new mode reverts after 15 s unless Return is pressed. Design: `docs/display-modes.md`.

## Mac

| Command | System | Leave with |
|---|---|---|
| `startmac -e 7.6.1` (default) | 7.6.1 | Special > Log Out (root: Apple menu) |
| `startmac -e 8.1` | 8.1 | Special > Log Out (root: Apple menu) |
| `startmac -e 6` | 6.0.7 with MultiFinder | Special > Logout |

A/UX 3.1's Mac environment (A/UX 2.0.1's for System 6). Root's System Folder is
`/mac/sys/System Folder`, other users' `~/System Folder` (8.1: `~/Mac/8.1`, 6: `~/Mac/6`); a new
template is copied over it at the next start, keeping your files. Root's Shut Down halts Unix.
`/etc/aux/macenv.conf` sets `TBMEMORY` (default a quarter of RAM, 8-32 MB) and, for root,
`MACMODEL`. 7.6.1 also runs on an Amiga host.

Known issues: System 6 is slow.

## TOS

`starttos` boots EmuTOS with the TeraDesk desktop and fVDI at the screen's size. EmuTOS's
BIOS and XBIOS routines are patched to call host services; no Atari chip is modelled.

- Environment `emutos`. Desktop: `desktop=emudesk` (default) or `desktop=xaaes` (XaAES,
  multitasking) in `~/TOS/.env`, or `-X` for one session; both use the same C:.
- C: `~/TOS`, G: games, U: `/`; more in `~/TOS/drives` or `-D L=dir`;
  `-i FILE` a FAT disk image instead.
- `-M` (implied on 1-bit screens) ST high monochrome, TT high on screens of 1280x960 or more; `-S` the ST screen, `-m MB` ST-RAM.
- `-C DIR` boots a program folder as single TOS: `starttos -C ~/TOS/PROGRAM`.
- `-rom FILE`: your own TOS ROM.
- Sound: DMA sound and the YM2149 (bell, key click, XBIOS music) on host sound. NVRAM settings
  are kept in `.nvram` in C:. Programs that write the video, palette or sound chips directly
  get no effect.

SMSQ/E is not available until it has an environment of its own.

## Amiga

`startamiga` boots Kickstart 3.0 or later to Workbench (environment `3.2`): `-rom FILE`, else an Amiga
host's own, else `/etc/amiga/kicka4000.rom` (`kicka3000.rom`). Any Picasso96 `rtg.library` build works; SYS: is `~/Amiga`. Fast RAM is up to
64 MB, fitted to free memory; `-m MB` (or `fastmb=MB` in `.env`) sets it. The display is RTG through Picasso96, 8-, 16- or
32-bit where the host has the mode; there is no chipset display. Sound goes through AHI.
Warp3D.library draws on a Voodoo3 when the screen is in its memory, else with software Mesa; games also get
640x480 and 800x600 screens, shown at the top left. 3D programs here and in X share the card and survive session switches.

## CP/M-68K

`startcpm` runs Concurrent CP/M-68K in the terminal. Ctrl-C stops the program inside; `EXIT`
(or closing the terminal) ends the session. `startcpm -c 1` (to 3) in
another terminal joins it as a further console. Drives are directories:
A: `~/CPM` (user areas 1-15 are subdirectories), H: home, M: a private scratch directory,
O: `/`, P: Digital Research's programs; `-D X=path` adds a drive. Environment `ccpm`
is `~/CPM`, others `~/CPM/NAME`. Z80 and 8086 CP/M programs run too.

## Win16

`startwin` runs Windows 3.x programs with its own KERNEL, USER and GDI and an x86
interpreter, as Wabi did. Install once from your own Windows 3.1/3.11 disks and Wabi 2.2:

```sh
startwin -install DISK1.IMG ... DISK6.IMG /cdrom/Wabi
```

Then `startwin` opens Program Manager, `startwin notepad.exe` one program. C: is `~/WIN16`
(environment `wfw311`; others `~/WIN16/NAME`), H: home, R: `/`. The Mac image has Windows for
Workgroups 3.11 in guest's `~/WIN16` and, given its disks, Windows 3.11 as `startwin -e win311`.
Design: `docs/win16-design.md`.

## X11

`startx` starts twm; `xsession` offers a menu, or `xsession twm|xview|mac|tos|amiga|win|console|cpm`.
With xdm the session chooser lists the same.
X draws 8-bit by default; `startx -- -depth 16` or `-depth 32` (or `DEPTH=16` in `/etc/default/x`)
gives a TrueColor screen where the display has that mode (Voodoo3, ZZ9000, Atari; on the Mac
when it booted at that depth, set in Monitors).
OpenGL is `libGL.so`, Mesa 5 in software; on a Voodoo3 it renders directly through the card's DRI driver,
`/usr/x11r6/lib/modules/dri/tdfx_dri.so` (Amiga images; `r200_dri.so` for a Radeon).

## Native ports

`startvoyage200` (package `v200env`) runs a native 68k host program:
`/usr/lib/voyage200/native`, or one you supply. Put the program in `~/Voyage200` as `native`
and its system media as `media`, or give `-x PROGRAM -m MEDIA`; the environment is `ams`;
`-e NAME` uses a named subdirectory.
For the Voyage 200, `media` is your Flash dump (AMS 2.09 and 3.10 tested); it boots from
reset to the AMS desktop. Archived variables are files in `archive/` beside it, in TI's
variable file format (`.v2e`, `.v2p` ...), so TI's link tools can exchange them; the dump is
never written. Cmd-Q quits.

## Lisa

`startlisa` boots your Lisa ROM and the Office System natively from the environment's folder; the
Lisa's chips are not modelled. Environment `office` is `~/Lisa`, others `~/Lisa/NAME`;
`tools/makelisa.sh NAME IMAGE` installs the files of your ProFile image (DC42 or raw 532-byte
blocks) there once, one host file per Lisa file, with a hidden sidecar `.NAME` holding its
catalog entry and header (the document's title). No disk image is opened at run time.
The folder also holds `lisa.rom` (`-r` names another), `parameter-memory` (`-p`) and `screen`
(`-s`); these options follow `--`. Lisa pixels are tall, so the screen shows at the host's aspect:
720x364 drawn 720x546, rows 3:2, columns 1:1, centred. Hosts narrower than 720 get it scaled down
to fit (640x480: 632x480). By default the environment takes the host's mode; `-s WxH`, or `lisa`
for 720x546, asks for the nearest listed mode at start. Without an 8-bit display it stops with a message. The host's power key is the
Lisa's power switch: the Office System shuts down, freeing the files it deleted, and the environment
ends. The Wastebasket holds one document; throwing away another deletes it. Cmd-Q, closing the session or SIGTERM press it too; if the OS has not shut down within
2 minutes, startlisa stops and says the volume was not closed. Design: `kernel/guest/mod/lisaguest/DESIGN.md`.

Known issues: a "micro diskette" dialog stays over the desktop.

## NeXT

`startnext` (package `nextenv`; status: Workspace) runs the `mach_kernel` of your OPENSTEP 4.2 CD natively from
`~/NeXT/openstep`, or `~/NeXT/NAME` with `-e NAME` (`tools/makenext.sh NAME` copies the CD there), with that folder as its root. It
boots through the first-boot panels to the Workspace Manager, logged in as `me` (no password).
For the login panel, Log Out and hold the mouse button until the panel shows; name `console`
gives the text console, `root` the Workspace as root. Host keys and mouse reach it, the pointer
draws at any screen width, a screen smaller than 1120x832 shows it scaled down and centred, and the File Viewer shows the host folder's free space. Timer, clock,
NVRAM, screen and sound (at the sound's own rate) are host services; copies between the kernel
and programs are host calls. No network yet. Cmd-Q quits.
