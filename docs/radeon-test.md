# Voodoo3 and Radeon test on a real Amiga

Thank you for testing. This disk runs AMIX (Unix for the Amiga) with drivers for a
PCI Voodoo3 and a PCI Radeon. Nobody has run them on a real card yet: whatever
happens, the photos and screenshots you send back tell us what to fix.

Test the Voodoo3 first (part A); it is the better-tested driver. Then swap in the
Radeon (part B).

## What changed in this build

Since build 68060-261017-16:

- One start command per environment: `startamiga` (was `startmig`),
  `startmac`, `starttos`. `-e name` picks an environment (e.g.
  `starttos -e xaaes`), `-d name` makes it the default, `-l` lists them.
- The loader loads the disk drivers (IDE, CyberStorm SCSI) as boot modules;
  the root is still the CyberStorm's SCSI ID 0.
- glxgears sends about a third fewer words to the Voodoo3 per frame (7566 to
  5170): its display lists replay straight to the card. Step A9 compares.
- Windows 3.1 programs: fixes to Program Manager groups and double clicks.
- Still open: BlitzQuake stops with "no context" (A4, issue #211).

Earlier: 68060 caches on (A8), 16-bit Workbench, IBrowse can save to SYS:,
`bigram` swap, faster GL, Quake and X.

Please check: `glxgears -info` names the Voodoo3 (A2), and each environment
starts.

## What you need

- **Amiga:** A4000 with a CyberStorm PPC or MK III (68060), 64 MB or more on
  the card.
- **Disk:** the disk at **SCSI ID 0** on the CyberStorm's SCSI, 512 MB or more.
  Its contents will be erased. On a ZuluSCSI, put `amix.hdf` on the SD card as
  the ID 0 image (e.g. `HD00_512.hda`) and keep your own ID 0 image aside. A
  CD-ROM at ID 1 can stay. The disk starts Unix by itself. Unix finds its root
  only at ID 0.
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

   Until its console starts, Unix fills the Amiga's own screen with one colour
   for each stage. If it stops on a plain colour, tell us which:

   | Screen | Where it stopped |
   |---|---|
   | the loader's text, unchanged | Unix never started (loader or copy) |
   | flashing white and red | the copied kernel is corrupt |
   | red | Unix started, sizing memory and boards |
   | yellow | building its memory map |
   | green | memory map on, starting up |
   | blue | starting the console |
   | text | console up; the stages above passed |

   Unix loads at 0x08000000, the start of the CyberStorm's RAM, and adds the
   motherboard's fast RAM below it when AmigaOS lists it; it needs none. With
   16 MB on the motherboard and 64 MB on the card, the console's `Total Unix
   memory` line reads about 80 MB.

   **If it stops on red or yellow**, reset (Ctrl-Amiga-Amiga) and boot AmigaOS
   from your own disk, or from the Early Startup menu. Unix leaves its progress
   in chip RAM at 0x1000 and again at 0x1FF000. Read it in a Shell:

   ```
   RexxMast
   rx AmigaBoot:amxmarks.rexx
   ```

   It prints one line per copy, e.g.
   `00001000 414D5842 n 37 mark 00000015 prev 00000013 arg 00104000 mem 15 00000000`.
   A line starting `414D5842` ("AMXB") is valid; photo or copy both lines.
   `mark` is the last step reached:

   | mark | step |
   |---|---|
   | 1, 2, 3, 4 | entry, memory list, memory sized, boards quiet (red) |
   | 5, 41 ('A') to 48 ('H') | memory map: start, tables, MMU on (43 twice), caches on (44), mlsetup (45-48) (yellow) |
   | 11 to 22, 30 | steps inside the memory map; 30 is clearing memory, `arg` the address reached |
   | 6 | memory map done (green) |

   To check the method first: after a normal boot to `login:`, reset and read
   the marks; `mark 00000006` means they survive a reset on your machine.

3. **Log in** as `root` (no password). A list of commands appears. Photo it.

If anything hangs, photo the screen, reset, and carry on with the next step.

## Part A: Voodoo3

A1. **Card check.** Type `drmtest`. Photo the output; the last line should be
    `drmtest: ok`.
A2. **X and 3D.** Type `startx`. In the window that opens, type `glxgears -info`.
    Photo the gears and the text. Wait 15 seconds and photo the frames per second.
    Ctrl-C. Then `LIBGL_FORCE_XMESA=1 glxgears` (software), wait 15 seconds, photo
    the numbers, Ctrl-C. Then `glprobe` and photo its lines (two photos if they
    scroll). A real Voodoo3 should show `lm lum = 150 90 30` and
    `mip bias 0 q 1: 0 1 2 3 4 5 6`; send the photo whatever it shows. Leave X:
    click the background, choose Exit.
A3. **Amiga environment.** Type `startamiga`.
    Workbench comes up inside Unix (this can take a few minutes), in 16-bit
    colour (Prefs > ScreenMode shows the depth). Open a Shell there and type
    `w3dtest`. Photo the picture and the text.
A4. **Amiga programs.** In the Workbench Shell of A3, type
    `execute SYS:Quake/quake +timedemo demo1` (BlitzQuake on Warp3D). Photo the
    error: it still stops with "no context", as its double-buffered screen does
    not yet fit the card memory a session has (issue #211); type `quit` to
    leave. Then open
    the IBrowse drawer and start IBrowse (the demo; close its reminder). Photo it.
    Ctrl + Left Alt + Left Amiga + 0, then Ctrl-C, ends Workbench.
A5. **Mac.** Type `startmac`. System 7.6.1 comes up (the first start takes a few
    minutes). Photo the desktop, open a few folders, photo again. Leave with the
    Apple menu, Log Out.
A6. **TOS.** Type `starttos`: the TeraDesk desktop. Photo it. Ctrl + Left Alt + Left Amiga + 0, then
    Ctrl-C ends it. Then `starttos -e xaaes` (XaAES): photo it and end it the
    same way. Then `starttos -C /TOS/CUBASE/CUBASE31`: Cubase 3.1 starts with no
    dongle question. Photo it and end it the same way. `SCORE` and `CAF`
    in place of `CUBASE31` start Score and Audio Falcon.
A7. **Quake.** Type `/usr/games/quake +timedemo demo1` and photo the frames per
    second at the end. Then run `/usr/games/glquake +timedemo demo1` inside X
    (A2) and photo that too.
A8. **68060 caches.** This build turns on the 68060's store buffer (ESB) and
    branch cache (EBC). Please compare both on (the default) with both off:
    run the GLQuake timedemo (A7), `/usr/games/quake -scale 1 +timedemo demo1`
    and `fbbench` each way, and photo the numbers. To turn them off, either
    set the long `cacr060` in the kernel file (SYS:unix) from 0x20800000 to 0
    (0x20000000 is ESB alone, 0x00800000 EBC alone) and reboot, or, live,
    write the full CACR value without those bits to both `sup_cacr` and
    `cacr` in kernel memory; the next interrupt loads it.
A9. **GL fast path.** Inside X (A2), run `glxgears` and
    `/usr/games/glquake +timedemo demo1` three ways and photo the picture and
    numbers each time: as they are; with `MESA_M68K=c` in front (the C code);
    with `TDFX_NOREPLAY=1` in front (no display-list replay). For example
    `MESA_M68K=c glxgears`. The pictures should look the same.
    Then `FX_FIFOCOUNT=60 glxgears` prints the words sent to the card per
    frame every 60 frames; photo a few lines.
A10. **Shut down** with `shutdown -y -g0 -i0`, switch off, and swap the Voodoo3 for
    the Radeon.

## Part B: Radeon

Boot as in Setup steps 2 and 3. With both cards in, first type
`echo 'CARDS="radeonds zz9000 voodoo3"' >> /etc/default/display` and reboot, so the
Radeon drives the screen.

B1. **Card check.** Type `radeontest 10`. Photo the whole output (two photos if it
    scrolls). During the test a red band appears at the top and a green rectangle
    in the middle.
B2. to B4. As A2, A3 and A7.
B5. Shut down as A10.

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
3. The frames-per-second numbers: glxgears card (default, `MESA_M68K=c`,
   `TDFX_NOREPLAY=1`) and software, the `FX_FIFOCOUNT` words per frame, Quake and
   glquake timedemo demo1.
4. Anything odd: wrong colours, a hang, a step that did not do what this sheet
   says.
