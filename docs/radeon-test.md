# Voodoo3 and Radeon test on a real Amiga

Thank you for testing. This disk runs AMIX (Unix for the Amiga) with drivers for a
PCI Voodoo3 and a PCI Radeon. Nobody has run them on a real card yet: whatever
happens, the photos and screenshots you send back tell us what to fix.

Test the Voodoo3 first (part A); it is the better-tested driver. Then swap in the
Radeon (part B).

## What you need

- **Amiga:** A4000 with a CyberStorm PPC or MK III (68060), 64 MB or more on
  the card.
- **Disk:** the disk at **SCSI ID 0** on the CyberStorm's SCSI, 512 MB or more.
  Its contents will be erased. On a ZuluSCSI, put `amix.hdf` on the SD card as
  the ID 0 image (e.g. `HD00_512.hda`) and keep your own ID 0 image aside. A
  CD-ROM at ID 1 can stay. The disk starts Unix by itself.
- **PCI bridge:** a Mediator for the A4000. Tell us which one exactly (the model and
  revision printed on the board, e.g. 4000Di MK-III or 4000D), its jumper settings
  and how it is powered.
  - Mediators for the A4000 need Buster revision 11.
- **Graphics cards:** a PCI 3dfx Voodoo3 2000 or 3000, then a PC Radeon 9200,
  9200 SE, 9250, 9000 or 7000 (64 or 128 MB).
  - Each needs its original PC BIOS. A Mac-flashed card will not work.
  - Voltage: look at the card's gold edge. Two notches (universal) work in any
    Mediator. One notch near the bracket end means 3.3 V only: it needs a Mediator
    with 3.3 V slots on ATX power. Tell us which kind each card is.
  - 256 MB Radeons often do not start on Amigas.
  - Only one graphics card in the Mediator at a time keeps things simple. With
    both in, the disk uses the Voodoo3; part B says how to pick the Radeon.
- **Monitor** on the card's VGA connector (not DVI).
- A phone camera: photos of the screen are the report.

Your cards keep their screens in their own memory, so you will not hit the "not
enough space" error a ZZ9000 can show after several display restarts.

## The files

| File | What it is |
|---|---|
| `amix.hdf` | the whole Unix disk, written raw to the spare disk |
| `TESTSHEET.md` | this sheet |

## Setup

1. **Disk.** Make `amix.hdf` the SCSI ID 0 disk: the ZuluSCSI image above, or
   on a PC `dd if=amix.hdf of=/dev/sdX bs=1M` to a whole disk.
2. **Start Unix.** Switch on. The disk's small AmigaBoot partition loads Unix;
   if it stops at a `[RETURN to continue ...]` line from `unix_boot040`, press
   Return. The screen goes to the graphics card. Photo the screen when it stops
   scrolling. If nothing shows on the card's monitor, photo the Amiga's own
   video output too.
3. **Log in** as `root` (no password). A list of commands appears. Photo it.

If anything hangs, photo the screen, reset, and carry on with the next step.

## Part A: Voodoo3

A1. **Card check.** Type `drmtest`. Photo the output; the last line should be
    `drmtest: ok`.
A2. **X and 3D.** Type `startx`. In the window that opens, type `glxgears -info`.
    Photo the gears and the text. Wait 15 seconds and photo the frames per second.
    Ctrl-C. Then `LIBGL_FORCE_XMESA=1 glxgears` (software), wait 15 seconds, photo
    the numbers, Ctrl-C. Leave X: click the background, choose Exit.
A3. **Amiga environment.** Type `startmig`. Workbench comes up inside Unix (this
    can take a few minutes). Open a Shell there and type `w3dtest`. Photo the
    picture and the text. Ctrl-Alt-0 goes back to the Unix console; Ctrl-C there
    stops it.
A4. **Quake.** Type `/usr/games/quake +timedemo demo1` and photo the frames per
    second at the end. Then run `/usr/games/glquake +timedemo demo1` inside X
    (A2) and photo that too.
A5. **Shut down** with `shutdown -y -g0 -i0`, switch off, and swap the Voodoo3 for
    the Radeon.

## Part B: Radeon

Boot as in Setup steps 2 and 3. With both cards in, first type
`echo 'CARDS="radeonds zz9000 voodoo3"' > /etc/default/display` and reboot, so the
Radeon drives the screen.

B1. **Card check.** Type `radeontest 10`. Photo the whole output (two photos if it
    scrolls). During the test a red band appears at the top and a green rectangle
    in the middle.
B2. to B4. As A2 to A4.
B5. Shut down as A5.

## What the screen tells you

| You see | It means |
|---|---|
| `voodoo3: 1024x768, 16 MB, ...` | the Voodoo3 was found and set up |
| `radeon: 5961, 128 MB, aperture 64 MB` (numbers vary) then `radeon: 1024x768, N frames` | the Radeon was found and set up |
| no `voodoo3:` or `radeon:` line at all | the card was not found: type `lspci` and photo it |
| picture rolls or the monitor says "out of range" | the screen timing is wrong: tell us the monitor's model |
| `drmtest: ok` | the Voodoo3's 3D device works |
| `radeontest` lines ending `ok`, last line `radeontest: ok` | everything the Radeon does for 3D works |
| `CP idle FAILED` | the Radeon cannot read commands from Amiga memory (bridge DMA) |
| `green clear FAILED` | commands work but drawing lands in the wrong place or colours |
| `texture upload FAILED` | byte order of pictures is wrong; the `texture bytes` line says how |
| `radeontest: no interrupt, clients poll` | fine: 3D still works |
| `glxgears` text names `Voodoo3` or `R200` | 3D drawn by the card; `Mesa X11` is software |
| `w3dtest` text names the card | Warp3D drawn by the card; `software` means it fell back |

## What to send back

1. The A4000's CPU card, RAM, Buster revision and AmigaOS version; the Mediator
   model, revision, jumpers and power; each card's make, model, memory and edge
   notches.
2. The photos from every step, in order, each named by its step (A2, B1, ...).
   Sharp, straight-on photos of the whole screen read best.
3. The frames-per-second numbers: glxgears card and software, Quake, glquake.
4. Anything odd: wrong colours, a hang, a step that did not do what this sheet
   says.
