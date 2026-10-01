# TOS apps on the disk image

Free programs `mktos.sh` installs from `ref/tosapps` (`TOSAPPS`), downloaded there by hand and never committed.

| App | Version | Licence | Source | On the image |
|---|---|---|---|---|
| Qed, GEM text editor | 5.0.5 (3f3, 68000 build) | free to copy and distribute, no fee of any kind (`readme.txt`: "public domain" since 3.09, sources included); so the disk image must be given away, not sold | https://github.com/freemint/qed, binary https://tho-otto.de/snapshots/qed/qed-5.0.5-3f3-000.zip | `/tos/sys/APPS/QED`, so `C:\APPS\QED\QED.APP` after `maketos` |
| Ballerburg, GEM game | 1999-2000 release of the 1987 game | freely copyable (`BALLER.PRG`: "Dieses Programm ist frei kopierbar"; no licence file); copying granted, modification not | https://www.eckhardkruse.net/atari_st/baller.html, binary https://www.eckhardkruse.net/atari_st/download/baller.zip | `/usr/games/tos/BALLER`, drive G: (`G:\BALLER\BALLER.PRG`) |

Unpacked layout: `ref/tosapps/qed/qed/` (zip contents) and `ref/tosapps/baller/` (`BALLER.*`).
