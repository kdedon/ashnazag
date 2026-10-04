# TOS container in the disk image

Apply from the repository root: `patch -p1 < images/tosenv/tos-image.diff` (live of 2026-09-30 21:55, kernel/guest-tos T1+T2 applied). New files: `mktos.sh`, `c-readme.txt`.

- `mkmacimage.sh` runs `mktos.sh` into `$MACW/tos`: `starttos`, `maketos`, `tosdrive`, EmuTOS 1.4 (from `ref/emutos-release`), cartridge, and as cpio archives the C: template `/tos/sys`, guest's `~/TOS` and the games of drive G: (`APPS.md`).
- `mac-diskroot.diff`: `/usr/bin/{starttos,maketos,tosdrive}`, `/etc/tos/{emutos.img,tosml.img}`, `/tos/sys`, `/usr/games/tos`, `/home/guest/TOS`; guest in group display, `/dev/tos` (c 56, 0660 root:display), `tosguest` in `/usr/aux/lib/mod.d`.
- `S05aux`/`auxreg`: third argument registers `tosguest` as major 56.
- `images/README.md`: "The TOS container" section.

Verified on a rebuilt image (kernel of 21:55): `starttos` reaches the EmuTOS desktop (drive C icon) in QEMU q800, `startmac` reaches the Finder. Image deleted. A user ROM is never included.

fVDI (`kernel/guest-tos/fvdi`): `mktos.sh` builds it into `/tos/sys` (so `maketos` and guest's `~/TOS` get it) and its source into `/tos/src/fvdi`; on a build failure it warns and the image has none. To boot without it, rename `C:\AUTO\FVDI.PRG` (e.g. `FVDI.PRX`) or run `starttos -S` (Ballerburg and other programs that clear, copy or read the screen themselves need it). `maketos -f` copies `FVDI.PRG` back. Programs that write the ST screen without changing its address or mode (VT52 text, Line-A) are hidden while fVDI runs.
