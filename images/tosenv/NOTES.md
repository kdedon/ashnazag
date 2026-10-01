# TOS container in the disk image

Apply from the repository root: `patch -p1 < images/tosenv/tos-image.diff` (live of 2026-09-30 21:55, kernel/guest-tos T1+T2 applied). New files: `mktos.sh`, `c-readme.txt`.

- `mkmacimage.sh` runs `mktos.sh` into `$MACW/tos`: `starttos`, EmuTOS 1.4 (from `ref/emutos-release`), cartridge, 1 MB drive C: with README.TXT.
- `mac-diskroot.diff`: `/usr/bin/starttos`, `/etc/tos/{emutos.img,tosml.img,c.img}`, `/dev/tos` (c 56, 0660 root:display), `tosguest` in `/usr/aux/lib/mod.d`.
- `S05aux`/`auxreg`: third argument registers `tosguest` as major 56.
- `images/README.md`: "The TOS container" section.

Verified on a rebuilt image (kernel of 21:55): `starttos` reaches the EmuTOS desktop (drive C icon) in QEMU q800, `startmac` reaches the Finder. Image deleted. A user ROM is never included.
