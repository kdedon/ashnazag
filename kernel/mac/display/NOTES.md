# Display service

`/dev/fb0`, `/dev/kbd`, `/dev/mouse` and sessions on the Quadra 800 kernel, as designed in
`docs/display-service-design.md` §2. The text console stays session 0 and the default.

## Files

| File | Role |
|---|---|
| `dsio.h` | user interface: ioctls, `fbinfo`, `fbmodeinfo`, `fbcmap`, `fbacq`, `fbstate`, `fbnote`, `inev`, `evinfo` |
| `ds.h` | kernel side: sessions, handles, the display |
| `ds.c` | core: display from `fbcons`, sessions, switch, CLUT, VBL, timestamps, ADB routing, hotkeys, panic hook |
| `dsdev.c` | cdevsw entries of majors 51 (fb), 52 (kbd), 53 (mouse) |
| `dsseg.c` | the mapping: segdev with fault, dup, unmap and free wrapped |
| `dspat.h` | test pattern and palette, shared by `dstest`, `t_display` and the host checker |
| `dstest.c` | user tool |
| `build.sh` | `ds.o` (kernel, no warnings, no common symbols) and `dstest` |
| `verify.sh` | static checks on the linked kernel |
| `diffs/` | changes to other files, `-p1` from the repository root (below) |

## Devices

| Node | Major | Minor 0 | Owner |
|---|---|---|---|
| `/dev/fb0` (`/dev/fb` link) | 51 | clone: every open gets its own minor | `root:display` (gid 25) 0660 |
| `/dev/kbd` | 52 | clone | same |
| `/dev/mouse` | 53 | clone | same |

A cloned minor is one open file. Its last close, which the kernel defers until its mappings are
gone, ends the session. The first open takes the screen `fbcons` found (`ds_init`).

## Ioctls implemented

| Ioctl | Notes |
|---|---|
| `FBIOGINFO` | boot mode; `FBF_VBL` once a DAFB VBL arrived; `fi_mode` = sResource mode id, 0x80 from a boot record |
| `FBIOGMODES` | the matched sResource's modes (`fbprobe.c` records them while it walks the declaration ROM); one entry after a boot-record boot |
| `FBIOSMODE` | `ENXIO` |
| `FBIOGETCMAP`, `FBIOPUTCMAP` | session only; `start + count <= fi_cmapsize`; 16-bit values kept, top 8 bits to the DAC at the next VBL |
| `FBIOACQUIRE`, `FBIORELEASE` | kind `FBK_USER`; `FBA_FRONT` as `FBIOSWITCH` |
| `FBIOSWITCH` | root, or the uid of the user session in front |
| `FBIOGSTATE`, `FBIOVBLWAIT` (1–600, `EINTR`), `FBIOGVBL` | |
| `FBIOBLANK` | black CLUT; the session's table comes back on unblank |
| `FBIOCACHE` | `FBC_WT` (DAFB default) or `FBC_CI` (NuBus default; `FBC_WT` there is `ENXIO`), for the next `mmap` |
| `EVIOCGINFO` | `EVK_ADB`, ADB handler id, `EVF_CAPSLATCH` |
| `EVIOCBIND` | fd of a session's `/dev/fb0`; resets the queue |
| `EVIOCGKEYS` | keys delivered down to the bound session |
| `EVIOCSLED` | bound session in front only |

Not implemented: `FBIOVIEW`, `DSIOCSHOTKEY`, `EVIOCBELL`, `EVIOCINJECT`, `/dev/conslog`, mode set.

## Sessions and switching

```
 ADB soft intr (IPL 1)          VIA2 CA1 (IPL 2)           tick callout
   ds_key: hotkey? ──qenable──┐   ds_vblintr: CLUT latch    VBL if DAFB is silent
   else route to front        │                             qenable if a switch waits
                              v
            queuerun (return to user mode, swtch)     ioctl / close (process)
                 ds_srv ─────────────┬─────────────────────┘
                                     v
   ds_switch: unload both sessions' user translations, VRAM -> old shadow,
   new shadow -> VRAM, fbcons base moves, CLUT, notes, synthetic key ups
```

- **Why queuerun.** A switch calls `hat_unload` on other processes' segments. STREAMS service
  procedures run only on return to user mode and in `swtch`, where no process is inside the HAT.
  `ds_srv` is the service procedure of a private queue; it lowers the IPL to 0 for the copy.
- **Shadow.** Each session has a kernel copy of the mapped region (`kmem_zalloc`, page-aligned),
  allocated at `FBIOACQUIRE`; the console's at the first user session. Hidden, a mapping faults
  onto shadow pages loaded with their page structures (`hat_memload`), so `hat_unload` finds them
  in the reverse map. In front, VRAM pages come through segdev (`hat_devload`).
- **Cache.** DAFB VRAM is registered with `hat_cm_fb_add`, so its user pages are cache-inhibited,
  non-serialized; `FBC_WT` rewrites the leaf entries to write-through after the fault. NuBus VRAM
  stays cache-inhibited, serialized. Every switch runs `cpusha dc` before and after the copies. Two
  mappings of one session with different cache modes see each other's writes late; that is the
  client's choice.
- **Races.** A generation count bumps at every switch; a fault that raced one unloads what it
  loaded and retries. Software locking (`F_SOFTLOCK`) is refused: physio into the frame buffer
  gets `EFAULT`. Only whole-segment `munmap` is accepted.
- **Hotkeys.** Control-Option-Command + 1…9: the n-th live session by creation order; + 0 or
  Escape: the console. The digit's down and up are swallowed.
- **Input.** Keys and mouse go only to the front session. At switch-out the old session gets an up
  for every key it saw go down (the console through `adbkbd_cons`); the new one never gets an up
  whose down it did not get. Records are the design's 16-byte `inev`, 256 per file, one
  `IE_DROP` on overflow. Time: `hrestime` plus the elapsed part of the tick from VIA1 T1's high
  byte (the low byte would clear the tick's interrupt flag), never decreasing.
- **End.** Last close or `FBIORELEASE`: translations unloaded, faults fail from then on; if it
  was in front, the next session by creation order, else the console. The slot is reused when
  its mappings are gone and it is not in front (a switch that failed is retried by the tick).
- **Panic.** `fb_own` calls `fbcons_panicfn` once; `ds_panic` points the console at VRAM and
  loads its CLUT, no copy.
- **VBL.** DAFB mask +0x104 = 4, clear +0x10C; VIA2 CA1 enabled at the first open. `p2int`
  serves port A bit 0 (SONIC) and bit 6 (video) until both lines are high (at most 8 rounds): CA1
  falls only when every slot line was high. Without DAFB VBLs for 4 ticks the tick stands in.
- **Console CLUT.** The Mac standard tables (8 bpp: 6×6×6 cube plus red, green, blue and grey
  ramps; fewer bits: grey), white at 0. Loaded when the console comes back.

## Diffs (`diffs/`)

| Diff | Change |
|---|---|
| `01-kernel-mac-relink-mac.sh.diff` | runs `display/build.sh`, links `ds.o` |
| `02-kernel-mac-macintr.s.diff` | `p2int`: VBL (port A bit 6) → `ds_vblintr`; SONIC and VBL served until both lines are high |
| `03-…-devtab-macdevsw.c.diff`, `04-…-checkimg.py.diff`, `05-…-NOTES.md.diff` | rows 51–53, checked as new drivers, major table |
| `06-…-video-fbcons.h.diff`, `07-…-fbcons.c.diff` | `fbcons_grab`, `fbcons_panicfn`, `struct fbpmode` |
| `08-…-video-fbprobe.c.diff`, `10-…-test-fbtest.c.diff` | `fp_modes`: the matched sResource's modes into `fbp_mode[]`; host test of the list |
| `09-…-video-verify.sh.diff` | DAFB constants only in the probe and the display service |
| `11-…-adb-adb.h.diff`, `12-…-adbkbd.c.diff` | `adbkbd_cons`; console repeat no longer stops for a key consumer |
| `13-…-ramdisk-root.manifest.diff`, `14-…-mkroot.sh.diff` | nodes, `dstest`, `/etc/group` with `display` |
| `15-…-diskroot-root.manifest.diff`, `16-…-mkdiskroot.sh.diff` | the same for the disk root |

Apply in the repository root: `for d in kernel/mac/display/diffs/*.diff; do patch -p1 < "$d"; done`,
then `sh kernel/build.sh` and `sh kernel/mac/display/verify.sh`.

## Tests

- `sh kernel/mac/display/verify.sh`: `p2int` dispatch; no switch, `hat_unload`, allocation or
  sleep reachable from the ADB, VBL, tick or panic paths; the panic hook; console repeat.
- `tests/src/t_display.c` (area `display`, 71 checks), in the default direct run. Host requests
  go over SCC channel B to `tests/display/hostio.py` (keys, mouse, screen dumps, comparison with
  the pattern rendered on the host). Dumps and references: `results/<run>/display/`.
- `tests/run-qemu.sh --net` runs `t_display` before `t_net`, so the SONIC works with VBLs on.

Results (QEMU q800):

| Run | Result |
|---|---|
| `run-qemu.sh --kernel-dir` | 512 pass, 0 fail, 2 skip; display 71/0/0 |
| `run-qemu.sh --net --kernel-dir` | display 41/0/3 (no host), net 22/0/0 |
| `run-qemu.sh --rom --kernel-dir` (A/UX Startup, 1 bpp) | 401 pass, 0 fail, 5 skip; display 41/0/3; the ROM root has no A/UX files (`aux.all` skipped), 812 KB free |
| ROM boot, disk root, `run-mac.sh` | login, `dstest info`, `draw` with CLUT rotation, `events` (keys, mouse), hotkeys 0, 1 and Escape; console text back |
| verify: integrate, devtab, video (with the ROM probe cases), adb (+ host test), sonic, rtc, display | 0 FAIL |

## Untested on hardware

Only QEMU's model has run these:

1. DAFB VBL: mask value 4 and clear by writing +0x10C (as QEMU models it); the slot line on port A
   bit 6. `ds_nhwvbl` counts DAFB VBLs, `ds_nswvbl` tick stand-ins.
2. CLUT at 1–4 bpp: entries written from index 0, as the ROM's 1-bpp reset does.
3. Write-through VRAM (`FBC_WT`) with DAFB bursts; `FBC_CI` is the fallback.
4. Enabling CA1 before the SONIC is up: a slot line held low by an unstarted SONIC stops VBLs;
   the tick then stands in.
5. `hat_cm_fb_add` takes one interval for VRAM; the HAT has two slots.

## Memory above 64 MB

Shadows come from `kmem_zalloc`, which maps into kvseg, a fixed 4 MB kernel heap at
0x40040000. The stock `kvm_init` also puts the page array and page hash there, 60 bytes per
page (about 2 MB at 128 MB), which leaves too little for several shadows.

`mac/patch_kvmpages.py` makes `kvm_init` use the array's address in a cached window onto the
first 16 MB of RAM (VA = PA | 0x60000000, built by `mac_iomap_build` with the kernel's RAM
cache mode). The identity map (DTT0) is noncacheable, so the window keeps the array cached as
kvseg did. `mac_ramwin_check` halts at boot if the array does not fit the window.

- RAM above 128 MB is ignored ("config: memory limited to 128 MB"): the segmap window
  (0x40440000-0x48440000) is sized from RAM and would overlap kvsegu.
- The heap still holds only a few shadows (0x79000 each at 640x480x8, more at higher depths).

## Known limits

- `FBA_FRONT` and `FBIOSWITCH` from a non-root user while the console is in front: `EPERM`, as the
  design rules (root or the owner of the front session). Otherwise any user in group `display`,
  logged in remotely, could take the screen and keyboard from the console. An X server started by
  a user from the console acquires without `FBA_FRONT` on `EPERM` and asks for the hotkey.
- The RAM-disk root image is rebuilt with `display` in `/etc/group` only when `mkroot.sh` runs
  (tape needed); the test roots get it from `tests/mktestroot.sh`.
- Panic with a user session in front is not exercised by the tests.
