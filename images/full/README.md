# Full test images (private)

q800-full.hda: main f4816920 with startnext (branch nextimg), 2026-10-19. The Falcon images: main 34db39d, 2026-10-14. They hold Apple, Atari, Commodore/Hyperion and DRI media: never publish or share.

| Image | Machine | Size |
|---|---|---|
| q800-full.hda | Quadra 800 / 68040 Mac, SCSI ID 0 | 1060 MiB |
| falcon-full-030.img | Falcon030, IDE (TOS 3/4 boots it) | 512 MiB |
| falcon-full-060.img | Falcon with CT60, IDE | 512 MiB |

Check with `sha256sum -c SHA256SUMS`, then write the image to the whole disk (not a partition), e.g. `dd if=q800-full.hda of=/dev/sdX bs=1M conv=fsync` or BlueSCSI/ZuluSCSI as HD00. Log in as `guest` (no password).

## q800-full.hda

System 7.6.1, Mac OS 8.1, System 6.0.7, Lisa (Office System 3.1 from your ROM and ProFile in ~/Lisa), Voyage 200 (AMS 2.09 in ~/Voyage200/media), TOS (EmuTOS, XaAES, TeraDesk, Cubase), Amiga (Kickstart 3.2, Workbench, IBrowse demo), Quake (/usr/games), CP/M-68K, Win16 (Windows for Workgroups 3.11, Windows 3.11 and Wabi 2.2 in ~/WIN16), NeXT (OPENSTEP 4.2 from the CD, in ~/NeXT/openstep), X11 with XView and GL. No SMSQ/E.

```
startmac                 System 7.6.1
startmac -e 8.1          Mac OS 8.1
startmac -e 6            System 6.0.7
starttos                 EmuTOS and TeraDesk; starttos -e xaaes
startamiga               Workbench
startcpm                 Concurrent CP/M-68K
startwin                 Windows for Workgroups 3.11 (Program Manager)
startwin -e win311       Windows 3.11
startlisa                Lisa Office System, scaled to the screen
startvoyage200           Voyage 200
startnext                OPENSTEP 4.2, scaled to the screen
startx / xsession        twm, or a session menu
/usr/games/quake         Quake; glquake in X
```

Cubase 3.1, Score and Audio Falcon are in guest's ~/TOS/CUBASE, patched (no dongle, EmuTOS-clean): starttos -C $HOME/TOS/CUBASE/CUBASE31.

Leave the Mac with Special > Log Out (System 6: Logout), Windows with Alt-F4, the Voyage 200 and NeXT with Cmd-Q, TOS, Lisa and X with Ctrl-C on the console.

Checked in QEMU at 640x480x8: login, X, Lisa, 7.6.1, 8.1, System 6, both Windows, TOS, Voyage 200, CP/M, NeXT. Known issues: CommandShell does not echo what you type (#234); Amiga stays on its start screen (#236); Lisa opens behind two diskette dialogs, Cancel both (#237). NeXT's first start shows the language panel (Return twice) and takes several minutes to the Workspace; it logs in as `me` by itself, also after Log Out.

Build (inputs in media/ and images/work):

```
AMIGASTAGE=images/work/amiga-stage QUAKE=images/work/quake/quake GLQUAKE=images/work/quake/glquake \
QUAKEPAK=media/quake/id1/pak0.pak XPKGS="xview gl" AMIX_TAPE=media/amix-tape \
ROOTMB=960 BOOT_SOURCE=images/q800-test-small.img sh images/macenv/mkmacimage.sh kernel/build/unix-mac.elf images/full/q800-full.hda
```

The Atari and Amiga kernels (kernel/build.sh with PLATFORM=atari, amiga) must be current: the packages carry their modules.

## falcon-full-030.img, falcon-full-060.img

System 6.0.7, TOS (EmuTOS; Cubase in ~/TOS/CUBASE), CP/M-68K, Win16 (startwin only), X11. The 060 image also takes a SuperVidel mode: bootline `'root=c?d0s1 sv=1024x768x16'`.

```
startmac6    System 6.0.7
starttos     EmuTOS and TeraDesk; -P this machine's TOS, -M mono
startcpm     Concurrent CP/M-68K
startwin     Windows 3.x programs (startwin -install your disks first)
startx       twm; or xsession
sndtest      a tone through the DMA sound
```

Ctrl-Alt-0 shows the console, Ctrl-Alt-1 to -9 the sessions (Mac: Ctrl-Option-Command).
