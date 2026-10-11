# TOS environment in the disk image

`mktos.sh` builds the environment's files into `$MACW/tos` (called by `images/macenv/mkmacimage.sh`): `starttos`, `maketos`, `tosdrive`, EmuTOS 1.4 (`EMUTOS`), the cartridge, and cpio archives of the C: template `/tos/sys`, the guest's `~/TOS` and the games of drive G: (`APPS.md`).

- Image paths: `/usr/bin/{starttos,maketos,tosdrive}`, `/etc/tos/{emutos.img,tosml.img}`, `/tos/sys`, `/usr/games/tos`, `/home/guest/TOS`. The guest is in group display; `/dev/tos` is c 56, 0660 root:display.
- `S05aux`/`auxreg`: the third argument registers `tosguest` as major 56 (`/usr/aux/lib/mod.d`).
- A user ROM is never included.

fVDI (`kernel/guest/tos/fvdi`): `mktos.sh` builds it into `/tos/sys` (so `maketos` and `~/TOS` get it) and its source into `/tos/src/fvdi`; on a build failure it fails unless `NOFVDI=1`. To boot without it, rename `C:\AUTO\FVDI.PRG` (e.g. `FVDI.PRX`) or run `starttos -S` (Ballerburg and other programs that clear, copy or read the screen themselves need it). `maketos -f` copies `FVDI.PRG` back. Programs that write the ST screen without changing its address or mode (VT52 text, Line-A) are hidden while fVDI runs.
