# Quadra 800 test disk

There are two images. Both boot the same way and start the same kernel. A third, `q800-unix.img`, boots the kernel straight from the ROM (see [Direct boot](#direct-boot)). `q800-unix-disk.img` adds a Unix root on disk (see [Disk with a Unix root](#disk-with-a-unix-root)).

| Image | Size | Contents |
|---|---|---|
| `q800-test.img` | 1,048,576,000 bytes (2,048,000 blocks) | The whole A/UX 3.1 disk: HFS, A/UX root, swap. You can still boot normal A/UX from it. |
| `q800-test-small.img` | 16,826,368 bytes (32,864 blocks) | Partition map, driver, and a 16 MB copy of the HFS volume. **No A/UX partitions**, so it cannot boot normal A/UX. |

`q800-test.img` is the A/UX 3.1 disk with our kernel added. The Mac boots System 7.1 from the disk's HFS partition, the Finder opens **A/UX Startup** (it is in Startup Items), and A/UX Startup starts our kernel on its own. You don't need to type anything.

The images are for local use only. They contain Apple's A/UX and System software, so do not publish them.

## What is on the disk

| Part | State |
|---|---|
| Partition map, driver, A/UX root, swap, "Eschatology 1" | Byte-identical to the A/UX 3.1 image. Our kernel does not mount the A/UX partitions. |
| HFS "Macintosh HD": `unix.coff` | New: our kernel (data fork only, type `COFF`, creator `SASH`), next to A/UX Startup |
| HFS "Macintosh HD": A/UX Startup | Autoboot command `launch -m -n unix.coff`. The A/UX file system check is replaced by a message. Countdown is 10 s. |

A/UX Startup's memory size stays at 512 KB. It loads kernels into free memory outside its own partition, up to 4000 KB. A larger Get Info size would leave less memory for the kernel.

## The small image

`q800-test-small.img` has three partitions:

| Blocks | Partition |
|---|---|
| 1–63 | Partition map (3 entries) |
| 64–95 | Apple_Driver, byte-identical to the A/UX disk |
| 96–32863 | Apple_HFS "MacOS", volume "Macintosh HD", 16 MB, about 7 MB free |

The HFS volume holds the same files as the big image. The build copies the catalog as it is, so every file and folder keeps its catalog ID, name, dates and Finder info. The volume keeps its name, creation date and boot blocks, and the System Folder stays blessed. Only the file positions change, so the Startup Items alias still finds A/UX Startup. The free space leaves room for a kernel up to A/UX Startup's 4000 KB limit.

A/UX Startup needs no A/UX partition for this boot. Its startup runs `autorecovery` and then `autolaunch`, and nothing else. The `echo` replaces the file system check, `launch -m` reads the kernel from HFS, and `launch` does not check the root device it passes to the kernel. Clicking OK in the Preferences directories dialog adds a `chroot` step before `autolaunch` for the rest of that session. Without an A/UX root the `chroot` fails with a message, and `autolaunch` still runs.

On the small image, stopping the countdown with Command-period still gives the `startup#` prompt. The A/UX commands in the next section do not work there: `fsck` and plain `launch` fail because there is no A/UX root.

## Writing the image

The big image is 1,048,576,000 bytes (2,048,000 blocks of 512 bytes). The small image is 16,826,368 bytes (32,864 blocks). The target disk must be at least as big as the image. A larger disk works, but the extra space is unused.

The image must go to the **whole disk** starting at block 0, not into a partition on it. On Linux (check the device name first, because this overwrites the whole disk):

```sh
sudo dd if=images/q800-test.img of=/dev/sdX bs=1M conv=fsync status=progress
```

For the small image, use `if=images/q800-test-small.img`.

On macOS, use `of=/dev/rdiskN` after `diskutil unmountDisk /dev/diskN`.

### SCSI2SD

In scsi2sd-util, set device 1 as follows:
- enabled
- **SCSI ID 0**
- start sector 0
- sector size 512
- sector count at least 2048000 (at least 32864 for the small image)

Then write the image to the raw SD card at offset 0 with the `dd` command above. Enable termination if the SCSI2SD is the last device on the internal cable.

### SCSI ID

The disk must be **SCSI ID 0**. That is the Quadra 800's internal disk ID, so remove the old internal drive or give it another ID. If the Mac starts from a different disk, reset PRAM: hold Command-Option-P-R at power-on until the second chime.

## What you will see

You need only the Quadra's built-in screen and an ADB keyboard. No serial cable is needed.

1. The Mac boots normally: "Welcome to Macintosh", the extensions, then the Finder.
2. A/UX Startup opens by itself and does two things in its window:
   - It runs `autorecovery`, which prints `A/UX file system check skipped`.
   - It runs `autolaunch`, which is `launch -m -n unix.coff`.
3. A/UX Startup shows its "Loading…" and "Launching…" messages.
4. The A/UX copyright box appears for **10 seconds**.
5. A/UX Startup switches the built-in video to its lowest depth (black and white, 1 bit per pixel) and jumps to the kernel.
6. From then on, all output is our kernel's console on the built-in screen, and input comes from the ADB keyboard.

## Stopping it or booting normal A/UX (big image only)

- **To stop the autoboot**, press **Command-period** while the copyright box is showing. You get the `startup#` prompt of A/UX Startup.
- **To boot normal A/UX from the prompt**, type:
  ```
  fsck -y -p /dev/default
  launch
  ```
  The first line is the root file system check that this image skips. `launch` with no arguments boots A/UX's own `/unix`.
- **To retry our kernel by hand**, type `launch -m -n unix.coff`. Add `-d` to dump the kernel info block first.
- **To make normal A/UX the default again**, open Preferences > Booting…, select "Check root file system", set the AutoLaunch Command to `launch`, and click OK. The Execute menu's Boot (Command-B) runs both steps.

## Rebuilding

```sh
sh images/mkimage.sh [kernel.coff]              # q800-test.img
sh images/mkimage.sh --small [kernel.coff]      # q800-test-small.img
```

The default kernel is `kernel/build/unix-mac.coff`. The script rebuilds from the zip each time and writes a sparse file. `--small` builds the big image in a temporary directory and makes the small one from it, and it leaves `q800-test.img` alone. It needs `unzip`, `python3` and `cc`. On first use it builds hfsutils into `toolchain/`. It checks its result in these ways:
- `hfsck` passes.
- `unix.coff` is read back and matches the kernel byte for byte.
- `elf2coff -c` accepts it.
- The A/UX Startup settings are printed.
- Everything outside the HFS partition hashes the same as the source image.

With `--small` it also checks:
- The partition map: the DDM block count equals the image size, the DDM driver entry points into the driver partition, every entry is consistent, and no partitions overlap.
- The driver partition and the map entries match the source, apart from sizes and positions.
- `hfsck` passes on the new volume.
- The boot blocks, the MDB and every catalog record match the original, apart from positions. Both forks of every file match byte for byte.
- `hls` listings with catalog IDs match, and a MacBinary copy of every file matches.
- `unix.coff`, `elf2coff -c` and the A/UX Startup settings, as above.

The files it uses:
- `auxsash.py`: reads and edits A/UX Startup's settings, and does the partition map and hash checks.
- `hfsfork.c`: rewrites a fork in place so the file keeps its catalog ID, which the Startup Items alias points to.
- `hfsshrink.py`: copies the HFS volume into a smaller one and compares two volumes.

## How the autoboot works (from A/UX Startup 3.1)

- **Settings.** Resource `SASH 1` holds the state. Byte 7 is autoBoot and bytes 8–11 are the Delay in seconds. Resource `SASH 2` holds the shell variables. Both are read at startup and saved back by Preferences > Booting….
- **Startup sequence.** When autoBoot is on, A/UX Startup types `autorecovery` into its shell. If that succeeds, it types `autolaunch`. Both are "automatic" variables, so each one expands to its value as a command line.
- **Countdown.** The copyright box is shown for Delay seconds inside the final launch step. Command-period in that box cancels the launch.
- **Video depth.** Just before the jump, A/UX Startup sets every open display driver to the mode with the smallest pixel size. So the kernel always starts in 1-bit mode on the built-in video. The low-memory `ScreenRow`/`ScrnBase` still describe the Monitors setting that was active before that switch.

## Direct boot

`q800-unix.img` needs no System file and no A/UX Startup. The ROM loads the disk's driver, runs our HFS boot blocks, and they load and start the kernel.

| Blocks | Partition |
|---|---|
| 1–63 | Partition map (3 entries) |
| 64–95 | Apple_Driver, byte-identical to the A/UX disk |
| 96– | Apple_HFS "MacOS", volume "Unix" (4 MB): our boot blocks and the kernel as file `unix` |

```sh
sh images/mkboot.sh [-c cmdline] [-m MB] [kernel.elf [out.img]]
```

The default kernel is `kernel/build/unix-mac.elf`. `-c` stores a kernel command line in the boot blocks, for example `-c root=c0d0s1`. The map and driver come from `q800-test-small.img`, so build that first. Write the image to the disk as above (SCSI ID 0, at least 8288 blocks) and make that disk the startup disk, or attach no other bootable disk: the ROM tries the PRAM startup disk first. To change the kernel, rebuild the image; the boot blocks hold the file's position and checksum. Details and the error ids (101–105, shown on a Sad Mac as 0x65–0x69) are in `kernel/mac/bootblk/NOTES.md`.

## Disk with a Unix root

`q800-unix-disk.img` (104,906,752 bytes, SCSI ID 0) boots like `q800-unix.img` and then runs AMIX multi-user from its own root partition. Log in on the screen and keyboard as `root` (no password). The previous build is `q800-unix-disk.prev.img`.

| Part | Contents |
|---|---|
| Kernel | `kernel/build/unix-mac.elf`, with the display service and A/UX shared-library support |
| Root (64 MB ufs) | AMIX 2.1 core, bsd and terminfo; `dstest`; `ping`, `route`, `arp`, `netstat` |
| `/usr/aux/bin` | A/UX 3.1 `ls`, `more`, `sh`, `sleep`, `vi` |
| `/shlib` | A/UX shared libraries (`libc_s`, `libc1_s`, `libmac_s`, `libmac1_s`, `libuucp_s`, `libX11_s`) |
| `/usr/aux/lib` | the guest modules, registered at run level 2 (`/etc/rc2.d/S05aux`) |
| `/usr/apkg` | apkg 0.2.2 and its engine package, installed; `/etc/inet/hosts` names `pkg.amigaux.org` |
| Swap | 32 MB |

### A/UX programs

Run them by path, for example `/usr/aux/bin/ls -l /` or `/usr/aux/bin/vi file`. They use the AMIX `vt100` terminal entries (`TERM=vt100`, set on the console). The A/UX `vt100` terminfo and termcap files are also in `/usr/aux/lib/terminfo` and `/usr/aux/etc/termcap` (select them with `TERMINFO` or `TERMCAP`).

### Network and packages

The Ethernet is not configured. Set the address, netmask and gateway in `/etc/inet/network-config` and remove the `#`s; it runs at boot. By hand:

```sh
/usr/sbin/slink addaen /dev/aen0 aen0
/usr/sbin/ifconfig aen0 192.168.1.80 netmask 255.255.255.0 up
/usr/sbin/route add default 192.168.1.1 1
```

AMIX has no resolver: the server's address is in `/etc/inet/hosts` (check it with `ping pkg.amigaux.org`), or put `nameserver=ADDRESS` in `/etc/apkg.conf`. Then:

```sh
apkg update                  # fetch the catalog
apkg list                    # or: apkg search WORD, apkg info NAME
apkg install gzip less       # dependencies too
apkg list installed
```

Most packages install into `/opt/amix/bin`; add it to `PATH`. apkg is set to use the stock `pkgadd` and `pkgrm` (`/etc/apkg.conf`). The engine package's `pkgadd` stops with a bus error in `pkginstall` once it has rewritten the package database itself.

### Rebuilding

```sh
sh kernel/mac/diskroot/pkg/getapkg.sh DIR                  # apkg datastreams, md5-checked
sh kernel/mac/diskroot/pkg/mkpkgimage.sh -p DIR WORK [kernel.elf [out.img]]
```

`mkpkgimage.sh` builds from a copy of `kernel/mac/diskroot` in `WORK` with `root.manifest` plus `pkg/root.manifest.add`. It needs the A/UX root named in `tests/aux/auxroot`, `tests/build/net07` (`tests/net/getnet.sh`) and the tape segments in `kernel/mac/diskroot/build/tape`. `-q` configures the Ethernet for an emulator's user-mode network (10.0.2.15, gateway 10.0.2.2).


## The Mac environment

`q800-mac.img` is the X disk image plus A/UX 3.1's Mac environment as A/UX installs it, with Mac OS 7.6.1 from its CD (`media/Mac OS 7.6.1.iso`, `CD761`) as the System Folder: `/mac` (CommandShell, TeachText, MacX, the Commando tools), the Finder's folders at `/` (Applications, Documentation, Shared Data) and `systemfolder`. Log in as `root` and type:

```sh
startmac
```

The Finder desktop appears in about a minute; the image carries the Finder's desktop database, so there is no rebuild.  Ctrl-Option-Command-0 shows the console, Ctrl-Option-Command-1 the Mac again.  Special > Logout quits back to the shell.  Faults the Mac handles itself print nothing; `macdiag` counts them (`quiet`).

The guest modules are registered at run level 2 (`/etc/rc2.d/S05aux`), which also starts the File ID daemon (`/etc/aux/fidd`).  root's System Folder is `/mac/sys/System Folder`; other users (group `display`) get `$HOME/System Folder` from `makemac` (`-f` copies the template over an existing one), and guest already has one; the Mac's PRAM is kept in `/etc/aux/pram`.

The Mac reports the model that fits the host and the ROM (a Quadra 800 on a Quadra 800) and gets a quarter of physical memory, 8 to 32 MB, as free swap allows.  `/etc/aux/macenv.conf` or the environment of `startmac` may set `TBMEMORY` (e.g. `16M`) and, for root, `MACMODEL` (a Gestalt machine ID, e.g. `22` for a Quadra 700).

Rebuild (needs the A/UX root in `tests/aux/auxroot`, the Quadra 700 ROM beside the repository, and the inputs of `x11/mkimage.sh`; the image goes to `$X11W/q800-mac.img`, default `images/work/x11`).  The last step boots the image once in QEMU with the Quadra 800 ROM to make the desktop database (`mkdesktop.py`, a few minutes; `DESKTOP=0` skips it):

```sh
sh images/macenv/mkmacimage.sh [kernel.elf [out.img]]
```

## The TOS container

The same image runs Atari TOS as an AMIX process. Log in as `guest` (or `root`) and type:

```sh
starttos
```

EmuTOS 1.4 (GPL-2) starts in its own display session; the desktop appears in a few seconds. Ctrl-Option-Command plus a digit switches display sessions: 0 is the console, 1 the first session started. End EmuTOS from the console with `kill`.

Drive C: is your folder `~/TOS` (AUTO, accessories, the saved desktop); `maketos` makes it from `/tos/sys`, and guest already has one. Without it C: is `/tos/sys`, read-only. G: holds the system's games (`/usr/games/tos`), U: the Unix tree; files are reached with your own permissions. `/tos/sys/drives` and `~/TOS/drives` name more drives (`G /usr/games/tos ro`), `tosdrive` edits yours, `starttos -D H=dir` adds one for a run, `-C dir` picks another C: folder, `-d FILE` a FAT image as C:. The sample apps are in `images/tosenv/APPS.md`.

`starttos -rom FILE` runs your own TOS ROM image instead. The image never contains one.

`/etc/rc2.d/S05aux` registers the `tosguest` module (`/dev/tos`, major 56, group `display`) with the A/UX modules. Rebuild with `images/macenv/mkmacimage.sh`; `images/tosenv/mktos.sh` makes the container's files.
