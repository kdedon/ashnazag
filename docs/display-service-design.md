# Display and input service

One kernel service owns the screens, the colour tables and the keyboards and mice. The
text console, the X server, A/UX's Mac environment and the guest containers are its
clients. Target: the Quadra 800 (DAFB, ADB) now, Amiga and Atari later, with one X
server binary for all three.

Status: design. Nothing here is built.

## 0. Summary

| Item | Decision |
|---|---|
| Frame buffer | `/dev/fbN`: info, mode list, CLUT get/put, `mmap` of VRAM, VBL wait, blank; mode set later |
| Input | `/dev/kbd`, `/dev/mouse`: 16-byte event records with native key codes, a key-code-set id and timestamps; no kernel auto-repeat |
| Ownership | **sessions**: every client owns one; one is in front per display. Hidden sessions keep drawing into shadow pages. Hotkey switch, CLUT and mode per session, console on crash or panic |
| X server | the existing **X11R6.3 AMIX port** ([`ref/x11r6.3-amix`](../ref/x11r6.3-amix)), plus a generic `fb` backend (mfb/cfb over `/dev/fb`) and an event-device input path. Apple's X11R5 `macII` DDX is reference only |
| Build | cross-build the server with our `m68k-cbm-sysv4` toolchain; use the port's natively built libraries and clients; native rebuild once as a check |
| Upstream | offer the `rtg` backend table, the `fb` backend and the event input to the port's repository |

Reference trees (not built):

| Path | Content |
|---|---|
| `ref/x11r5/` | X11R5 public-patch-26 `mit-1/2.tar.Z`; extracted `server/{ddx/macII,ddx/mi,ddx/mfb,ddx/cfb,dix,os,include}`, `include`, `config` |
| `ref/x11r6.3/` | X11R6.3 `xc-1..3.tar.gz` (same SHA-256 as the port's `SOURCES.sha256`), `fix-01..03`; extracted `config`, `include`, `programs/Xserver` (no XIE/PEX/Xprint/xfree86), `programs/xkbcomp`, `lib/xtrans` |
| `ref/xfree86-3.3.6/` | `X336src-1.tgz`; extracted `Xserver/{afb,ilbm,iplan2p2,iplan2p4,iplan2p8,cfb24,hw/xfree68,hw/sun}`, xkb `keycodes`/`symbols` for macintosh, amiga, ataritt |
| `ref/x11r6.3-amix/` | the AMIX port (read-only clone) |

## 1. What exists

| Piece | State |
|---|---|
| Console | `kernel/mac/video`: 8×16 text renderer at 1–32 bpp on the mode the firmware left; `fbprobe.c` finds it (boot record, or A/UX low memory + declaration ROM sResources, sense lines); resets the 1-bpp CLUT (index register `0xF9800200` long 0, then R, G, B bytes at `+0x213`); masks the DAFB VBL interrupt (`+0x104`, `+0x10C`) |
| ADB | `kernel/mac/adb`: key consumer `(unit, KC_CHAR, code, more)`, code = raw ADB, bit 7 up; mouse consumer `(unit, MOUSE_CHANGE, r0, changed)`; console path with its own auto-repeat |
| VRAM mapping | kernel: DTT1, supervisor, cache-inhibited. No user mapping yet |
| A/UX Startup | leaves every display at its **lowest depth (1 bpp)**; a direct boot (QEMU `-kernel`) keeps the Monitors depth |
| Mac environment | designed ([aux-kernel-design.md](aux-kernel-design.md) §6.6–6.9): fake NuBus card + Slot Manager over a synthetic declaration ROM, kernel cursor at VBL, `ui_key_intr`/`ui_mouse_intr` |
| Containers | designed ([guest-container-design.md](guest-container-design.md) §4.2, §10.3): surfaces, zero-copy or converted, full-screen switching by hotkey |

## 2. Kernel service

### 2.1 Structure

```
  console (fbcons)   X server    uinter (Mac env)   guestsvc (containers)   test tools
        |               |               |                  |                   |
   in-kernel        /dev/fbN        in-kernel          in-kernel           /dev/fbN
    session       /dev/kbd,mouse     session            session          /dev/kbd,mouse
        \______________ |_______________|__________________|___________________/
                                   ds core
        sessions, shadow pages, fault-time mapping, CLUT/mode per session,
        input routing to the front session, hotkeys, key-code translation
                    |                                   |
            display backends                      input backends
   dafb (Q800), nubus (declaration ROM),     adb (Mac), amiga keyboard/port,
   amiga chipset / RTG, atari shifter/Videl  atari IKBD
```

- `ds` core: platform-independent, built into the kernel (the console needs it at boot).
- Backend ops per display: `info`, `modes`, `setcmap`, `getcmap`, `setmode` (optional), `vblwait`/`vblhook` (optional), `blank` (optional), `pfn(offset)`.
- In-kernel clients use the same session calls as the device: `ds_open(disp, kind, ops, name)`, `ds_close`, `ds_front`, `ds_setcmap`, `ds_input` callback. `ops` = `{show, hide, input, mode}`.

### 2.2 `/dev/fbN`

One node per display (`/dev/fb0` built-in DAFB, further nodes for NuBus cards); `/dev/fb` links to the main one. Ioctls are `('F'<<8)|n`, data copied in and out by the driver, as AMIX's own drivers do.

| Ioctl | Arg | Meaning |
|---|---|---|
| `FBIOGINFO` | out `struct fbinfo` | current mode of the caller's session |
| `FBIOGMODES` | in/out `{count, struct fbmodeinfo *}` | modes the display offers (DAFB: from the declaration-ROM sResources `fbprobe.c` already walks) |
| `FBIOSMODE` | in mode id | session owner only; takes effect now if in front, else at switch-in. Returns `ENXIO` until the backend has `setmode` |
| `FBIOGETCMAP`, `FBIOPUTCMAP` | `struct fbcmap {u_short start, count; u_short *r, *g, *b}` | 16-bit components (X's range); the backend keeps the top `fi_cmapbits`. Written to the hardware at the next VBL when the backend has one, else at once; kept in the session when hidden |
| `FBIOACQUIRE` | in `struct fbacq {kind, flags, name[16]}`, out session id | makes the fd a session. `FBA_FRONT`: switch to it now (allowed as for `FBIOSWITCH`) |
| `FBIORELEASE` | – | ends the session (also on last close) |
| `FBIOSWITCH` | in session id, 0 = console | root, or the owner of the session in front |
| `FBIOGSTATE` | out `{session, front, serial}` | |
| `FBIOVBLWAIT` | in count | sleep until the n-th next VBL (`EINTR` on signal) |
| `FBIOGVBL` | out counter | VBL count |
| `FBIOBLANK` | in on/off | screen saver: hardware blank if the backend has it, else a black CLUT (indexed modes) |
| `FBIOCACHE` | in `FBC_WT`/`FBC_CI` | cache mode for this session's next `mmap` |
| `FBIOVIEW` | in session id | the next `mmap` maps that session's backing read-only (viewers, §4.1, §5) |
| `DSIOCSHOTKEY` | in hotkey table | root only |

```c
struct fbinfo {
	u_long	fi_type;	/* FBT_DAFB, FBT_NUBUS, FBT_AMIGA_OCS, FBT_AMIGA_RTG, FBT_TT, FBT_VIDEL */
	u_long	fi_layout;	/* FBL_PACKED, FBL_PLANES, FBL_ILBM, FBL_IPLAN2 */
	u_long	fi_width, fi_height;
	u_long	fi_depth;	/* bits per pixel */
	u_long	fi_rowbytes;
	u_long	fi_planebytes;	/* FBL_PLANES: distance between planes */
	u_long	fi_visual;	/* FBV_MONO (0 = white), FBV_PSEUDO, FBV_TRUE, FBV_DIRECT */
	u_long	fi_rmask, fi_gmask, fi_bmask;
	u_long	fi_cmapsize, fi_cmapbits;
	u_long	fi_offset;	/* first pixel, from mmap offset 0 */
	u_long	fi_size;	/* bytes to map */
	u_long	fi_mode;	/* current mode id */
	u_long	fi_flags;	/* FBF_VBL, FBF_BLANK, FBF_SETMODE */
	u_long	fi_mmwidth, fi_mmheight;
	char	fi_name[16];
};
```

`mmap(fd, off 0, fi_size)` needs a session. The mapping is a small segment driver whose fault handler maps the session's current backing: VRAM PFNs while in front, the session's shadow pages while hidden. A switch unloads the translations of both sessions (`hat_unload` over the segments it tracks); the next access faults onto the new backing. A process never reaches another session's VRAM contents through its mapping.

**Cache mode (040).** Default **write-through** for built-in VRAM: reads (copy area, cfb read-modify-write) hit the cache, writes reach VRAM at once. The CPU is the only writer; while a session is hidden the kernel writes VRAM through its cache-inhibited alias, so every switch-in pushes and invalidates the data cache (`cpusha dc`, once per switch). **Cache-inhibited, non-serialized** for NuBus cards, Amiga chip RAM (the blitter writes it) and any 030 (logical-tagged caches). Never copyback. Measure both in D3.

### 2.3 Input devices

`/dev/kbd` and `/dev/mouse` merge all keyboards and all pointing devices. A reader receives events only while its session is in front: `EVIOCBIND(fbfd)` ties the input fd to the fb session (an input fd without a session gets nothing). In-kernel sessions get the same records through `ops->input`.

```c
struct inev {			/* 16 bytes */
	u_char	ie_type;	/* IE_KEY, IE_REL, IE_BTN, IE_ABS, IE_SYN, IE_DROP */
	u_char	ie_unit;	/* which keyboard or mouse */
	u_short	ie_code;	/* key: native code; rel/abs: axis; btn: 1..n */
	long	ie_value;	/* key/btn: 1 down, 0 up; rel: delta, y down positive */
	long	ie_sec, ie_usec;
};
```

- **Key codes are native**: ADB codes 0x00–0x7F (power key 0x7F), Amiga raw keys, IKBD scancodes. `EVIOCGINFO` returns `{kset: EVK_ADB, EVK_AMIGA, EVK_IKBD; ADB handler id / keyboard type; flags: EVF_CAPSLATCH}`. ADB Caps Lock latches: it reports down when locked and up when released, so the flag lets X treat it as a toggle.
- **Mouse**: ADB talk-R0 → `IE_BTN 1`, `IE_REL X`, `IE_REL Y`, `IE_SYN`. Extended mice (more buttons) later. Acceleration in user space.
- **Timestamps**: `hrestime` plus the VIA1 T1 fraction of the current tick, taken in the interrupt.
- **No kernel auto-repeat** for readers; X and uinter repeat themselves. The console keeps its own.
- Queue 256 records per reader; on overflow one `IE_DROP` and the reader calls `EVIOCGKEYS`.
- `EVIOCGKEYS` (bitmap of keys down), `EVIOCSLED`, `EVIOCBELL {percent, hz, ms}` (Mac: ASC tone; else screen flash by the client), `EVIOCINJECT` (§5.3).
- Switch hygiene: at switch-out the old session gets `up` for every key it saw go down; the new session never gets an `up` whose `down` it did not get (the hotkey chord included).
- `ds_kxlate(kset → ADB)`: one kernel table per key-code set, used by uinter and the Mac guest profile on non-ADB hosts ([aux-kernel-design.md](aux-kernel-design.md) §6.6).

### 2.4 Sessions and switching

| Rule | |
|---|---|
| Kinds | console (session 0, always present), user (fd holders: X, test tools), mac (uinter), guest (guestsvc) |
| Front | one session per display. Input follows the front session of the main display |
| Hidden session | keeps running; its mapping points at shadow pages (`fi_size`, 1 MB at 1152×870×8); its CLUT and mode are kept; `read()` on its fb fd returns `FBN_HIDDEN`, `FBN_SHOWN`, `FBN_MODE` notes (`poll` POLLIN), so a client may stop drawing |
| Switch | unload both mappings; VRAM → old shadow, new shadow → VRAM (skipped for a session that has never been in front: cleared instead); set mode, load CLUT, flush cache, notify, reroute input |
| Hotkey | taken by `ds` before routing, never delivered. ADB default: Control-Option-Command + 1…9 (session by creation order), + 0 console. Amiga default: Control-Alt-Left Amiga + digit. Atari: Control-Alternate + digit. Table settable by root (`DSIOCSHOTKEY` on `/dev/fb`) |
| Emergency | Control-Option-Command-Escape (ADB): console to front whatever the owner does |
| Crash | last close of a session's fd (exit, kill, crash) releases it: the next session in order, else the console, comes to front with its own mode and CLUT |
| Panic | `fbcons` takes VRAM directly (as now), after `ds_panic` sets the console's mode and CLUT without copying |
| Console hidden | `fbcons` draws into its shadow (its `fm_base` moves), so the text survives; messages also go to the SCC and `/dev/conslog` |

### 2.5 Permissions

- `/dev/fb*`, `/dev/kbd`, `/dev/mouse`: `root:display`, mode 0660. Users in group `display` may create sessions. The X server needs no set-uid.
- Session calls act only on the caller's own session. `FBIOSWITCH`: root or the front session's owner. Hotkey table: root.
- `FBIOVIEW` (read-only mapping of another session's backing) and `EVIOCINJECT` (post events to another session): same uid or root (§5.3, §6).
- Raw VRAM is reachable only through a session; `/dev/mem` is not needed.

### 2.6 DAFB backend (Q800)

| Function | Q800 |
|---|---|
| info, modes | `fbprobe.c` results; mode list from the declaration ROM's DAFB sResources |
| CLUT | index register `0xF9800200` (long), data `+0x213` (R, G, B bytes), as `fp_clut1` does |
| VBL | unmask DAFB interrupt (`+0x104`), clear at `+0x10C`; VIA2 slot line (A/UX: port A bit 6). Counter, `vblwait` sleepers, per-VBL hooks (CLUT latch, uinter cursor). *(Verify QEMU raises it; else fall back to the 60 Hz tick)* |
| Blank | unknown register; black CLUT until found |
| Mode set | **D5**: DAFB timing/stride/depth registers and the DAC mode, derived from the ROM's DAFB driver `SetMode`; the declaration ROM gives the VPBlocks. Until then every session uses the boot mode |
| Depths | 1, 2, 4, 8 bpp; 16 bpp (x-5-5-5) where VRAM allows. The Q800 has 512 KB VRAM, 1 MB with SIMMs *(verify)*; 24/32 bpp at 640×480 needs 1.2 MB, so likely not on a Q800 *(verify in the Q800 ROM mode table)*. QEMU's `-g WxHxD` offers 1–8 and 24 bpp *(verify 16)* |

NuBus cards: the same backend shape, CLUT through the card's documented registers only when known; otherwise read-only (mode as the ROM left it, no CLUT).

## 3. X server

### 3.1 Which server

| Option | Build with our tools | Cost on a 25–33 MHz 040 | Fit |
|---|---|---|---|
| **X11R6.3, AMIX port** | yes: GCC 2.7.2.3, AMIX shared-lib ABI already solved | ~2 MB server, mfb/cfb draw straight into VRAM; Xsun-class performance, several times a Sun-3 | matches amigaux.org's planned X11R6.3; runs xterm/xclock/twm today (on ZZ9000) |
| XFree86 3.3.6 | yes (R6.3-based) | as R6.3 | its `hw/xfree68` needs the PC-centric `hw/xfree86` common layer and Linux fbdev. Useful as a **source** of `afb`, `ilbm`, `iplan2p*` (planar Amiga/Atari) and xkb keymaps, all MIT-style |
| XFree86 4.x | needs egcs/2.95+, own module loader, Linux/BSD os-support | 5–8 MB, slow start; nothing gained on a dumb frame buffer | no |
| X.org, `Xfbdev`/kdrive or `Xorg -fbdev` | modern GCC has no m68k SVR4 target; C99, meson, pixman, xcb | 15–30 MB RSS; Render in software, slow | only if amigaux ships a modern GCC; revisit then |

Recommendation: **X11R6.3 through the AMIX port.** Clients speak the protocol, so R4, R5 and R6.3 clients (stock AMIX, amigaux packages) all run against it. Mesa's Xlib driver needs no server GLX.

### 3.2 The AMIX port as base

`overlay/xc/programs/Xserver/hw/amix` (read in `ref/x11r6.3-amix`):

- `amixInit.c`: probe table `amixFbData[] = {amixMono (AMIX /dev/screen, 1 plane), TIGA, rtg}`; with RTG built in, every head is forced to `rtgProbe`/`rtgCreate`.
- `rtg/rtgInit.c`: screen private, `AddScreen`, software cursor (miDC), colormap; **calls ZZ9000 functions directly** (`ZZ9000Probe`, `ZZ9000_selected_mode`, `ZZ9000InitHW`, `ZZ9000ScreenInit`) and fixes the formats at depth 16 + 1.
- `rtg/zz9000`: ~2,300 lines, its own GC and span code for RGB565 (mi fallbacks, tiles/stipples as solid), `/dev/zz9000` ioctls `GETINFO/SETMODE/SETSWITCH/FILL/PROBE/GETFBINFO/FILLRECT/COPYRECT`.
- Input: `rtgOpenInput` opens an AMIX console screen through `libscreen` (`OpenScreen`, `SIOCACTIVATE`, `SIOCSETINPUTMODE SIM_RAWKEY`); `amixIo.c` reads 8-byte `struct inputevent {type, class, code, qualifiers}` records (Amiga raw keys, bit 7 up; mouse move `code` = dx<<8 | dy; buttons as codes 0x7C–0x7E), repairs missed qualifier changes, timestamps them itself, and hands `struct InputEvent` to `amixKbd.c`/`amixMouse.c` (`key = (code & 0x7F) + 1`, Amiga keymap in `amixKeyMap.c`, DDX auto-repeat).
- Build: native, `amix.cf` (`-DAMIX -Dm68k -DSVR4`, `-lsocket -lnsl -ldbm -lscreen`, XKB built, MIT-SHM off), `mfb/maskbits.h` inline-asm path disabled, `servermd.h` m68k padding, launcher `startXzz9000` with `-kb` (XKB device path unfinished).

### 3.3 Frame-buffer backend: generic `fb`, not a ZZ9000 clone

**Do not mimic `/dev/zz9000`.** Its ioctls are card-specific (Zorro fields, capture/RTG switch, blit acceleration, fixed 0x10000 offset, RGB565 only, pitch = width × 2) and the ZZ9000 code draws with its own GC layer. A DAFB is a dumb linear buffer, which R6.3's own mfb and cfb already drive completely and faster than span fallbacks.

1. **`rtg` ops table** (upstream refactor, ~150 lines): `struct rtgcard {name, probe, inithw, closehw, screeninit, createdefcmap, formats}`; `rtgInit.c` calls through it; ZZ9000 becomes one entry, unchanged inside.
2. **`rtg/fb`** (~600 lines) over `/dev/fb`:
   - probe: open `/dev/fbN` (`-fb /dev/fbN` option), `FBIOACQUIRE` (kind user, `FBA_FRONT`), `FBIOGINFO`, optional `FBIOSMODE` for `-depth`;
   - map: `mmap` `fi_size`, pixel base = map + `fi_offset`;
   - screen: depth 1 → `mfbScreenInit` (StaticGray, pixel 0 white as on the Mac; `FlipPixels` or a 2-entry CLUT); 8 → `cfbScreenInit` (PseudoColor, plus StaticGray/GrayScale); 16 → `cfb16` (TrueColor, masks from `fi_*mask`, x-5-5-5); 32 → `cfb32`; planar layouts → `afb`/`ilbm`/`iplan2p*` (§7);
   - colormap: install/store → `FBIOPUTCMAP` (whole-map writes like macII's `SetEntries`, but incremental ranges are allowed);
   - notes: add the fb fd to the select set; on `FBN_SHOWN` nothing is required (the kernel restored VRAM and CLUT); on `FBN_MODE` reset the screen;
   - save screen → `FBIOBLANK`; close → `FBIORELEASE`.
   The ZZ9000 could expose `/dev/fb` too; then `rtg/fb` drives it unaccelerated with no card code.
3. **Formats**: `rtgCreate` takes the formats from the card entry (1 + the screen depth), not fixed 16 + 1.

### 3.4 Input: event devices, not emulated Amiga screens

Option A — give the Mac an AMIX-style `/dev/screen` input (ADB → Amiga raw keys, qualifiers) so `amixIo.c` runs unchanged. Rejected: needs a `libscreen`-compatible screen driver on the Mac; Mac keys without Amiga equivalents are lost (keypad `=` and Clear, F13–F15, Power, the second Command); no timestamps; the qualifier-repair hack stays.

Option B — **recommended**: a small reader `amixEv.c` (~300 lines) plugged into the existing `KbPriv.GetEvents` hook, chosen when `/dev/kbd` exists (our kernel), with the `/dev/screen` path kept for stock AMIX kernels:

- `IE_KEY` → `struct InputEvent` key record; `IE_REL` pairs up to `IE_SYN` → one move record; `IE_BTN` → button record; kernel timestamps kept.
- Keymap by `kset`: `EVK_AMIGA` → `amixKeyMap.c` as today (`code + 1`); `EVK_ADB` → new `amixMacKeyMap.c` (~200 lines; keycode = ADB + 8, the offset of macII and of XKB's `keycodes/macintosh`; Command = Meta/Mod1, Option = Mode_switch; Caps Lock as a toggle when `EVF_CAPSLATCH`); `EVK_IKBD` → Atari table (XKB `ataritt`, + 8).
- One-button mice: macII's emulation as an option (`-mb3`): left/right arrow = buttons 2/3, Option + arrow = arrow.
- Bell → `EVIOCBELL`; LEDs → `EVIOCSLED`.
- On `FBN_SHOWN`, `EVIOCGKEYS` resynchronises modifiers.

### 3.5 A/UX dependencies of `XmacII` and where they go

From Apple's `macII` DDX (X11R5) and the A/UX `XmacII` binary's strings; the A/UX headers give the ioctl meanings.

| A/UX | Use in `XmacII` | Here |
|---|---|---|
| `/dev/console` + `I_POP` of all modules, `TCSETA` raw, `FIONBIO`, `FIOASYNC` + `F_SETOWN` (SIGIO) | keyboard/mouse byte stream | `/dev/kbd`, `/dev/mouse`, non-blocking, in the select set (R6.3 needs no SIGIO) |
| `VIDEO_RAW` | ADB key codes, bit 7 up | `IE_KEY`, native ADB code |
| `VIDEO_MOUSE` / `VIDEO_NOMOUSE` | mouse as `MOUSE_ESCAPE` 0x7E + 2 bytes (R0: button bit 7, 7-bit dy, dx) in the key stream | `IE_BTN`/`IE_REL`/`IE_SYN` on `/dev/mouse` |
| `VIDEO_MAC`, `VIDEO_ASCII`, `I_PUSH line`, `TCSETA` restore at exit | give the console back | `FBIORELEASE` / close; the kernel restores console mode and CLUT |
| `CONS_REDIRECT`/`CONS_UNDIRECT`, `/dev/osm` | kernel messages away from the screen, readable by a client | hidden console draws into its shadow; messages to SCC and `/dev/conslog` |
| `slotmanager()` `_sNextTypesRsrc`, `_sFindStruct`, `_sGetBlock` (mVidParams); SIGSYS if absent | find the video sResource and every mode's VPBlock | `FBIOGMODES` (kernel parses the declaration ROM) |
| `VIDEO_MAP_SLOT` (16 MB segment above 128 MB) or `phys()` | map the slot | `mmap(/dev/fbN)` |
| `VIDEO_DATA` (`struct video_data`) | geometry of the current mode | `FBIOGINFO` |
| `VIDEO_CONTROL` csCode `SetMode` (sResource mode id) | pick a depth | `FBIOSMODE` (D5) |
| `VIDEO_CONTROL` csCode `SetEntries` (`ColorSpec[256]`, start 0, count − 1) | whole CLUT at once | `FBIOPUTCMAP` |
| `VIDEO_BELL` | beep | `EVIOCBELL` |
| `macIIBlackScreen`, SaveScreen | clear, saver | cfb clear, `FBIOBLANK` |
| slots 9–E, `-screen n -depth d`, Zaphod | multiple cards | `/dev/fb0…`, `-fb` per screen, the port's Zaphod code |

Mac knowledge reused from `macII`: 1-bpp polarity (0 white), whole-map CLUT loads, depth choice from the mode list, ADB keymap and Option-arrow handling, three-button emulation.

### 3.6 Build

| Route | Cost | Use |
|---|---|---|
| **Cross, server only** | host `imake`/`makedepend` from `xc/config`; an `amix.cf` variant (`CcCmd m68k-cbm-sysv4-gcc`, cross `ar`/`ld`, host `cpp -traditional` for imake, `CrossCompiling YES`, "not fully supported" in R6.3); 3–5 days to set up; then minutes per build | development. The server links static archives plus AMIX's `-lsocket -lnsl -ldbm -lscreen` from the sysroot, so the `.so`/`.sa` shared-data ABI (native `ld -G -h`) does not arise |
| Native in QEMU `q800` on our kernel | server only: several hours; full tree with libraries and clients: a day or more *(estimate; a 25 MHz 040 takes ~10 h for a full R6 tree)* | once, as the reproducibility check and the upstream build, and as a kernel stress test (fork/exec-heavy make) |
| Native on the port's AMIX | as upstream does | libraries and clients (`/usr/x11r6`), unchanged |

Fonts, `rgb.txt` and XKB data come from the port's install or amigaux packages. XKB stays off (`-kb`, core keymap from the DDX) until the port's XKB device path works.

### 3.7 Effort

| Work | Size | Time |
|---|---|---|
| `rtg` ops refactor | ~150 lines | 2 days |
| `rtg/fb` backend, 1/8/16/32 bpp | ~600 lines | 4–5 days |
| `amixEv.c` + `amixMacKeyMap.c` | ~500 lines | 3–4 days |
| cross-build setup | config | 3–5 days |
| bring-up in QEMU (D2, D3) | | 1–2 weeks |
| **X total** | | **~4–5 weeks** (a from-scratch DDX: 6–8) |
| kernel service D1 (fb, input, sessions, DAFB VBL/CLUT) | ~2,500 lines | 2–3 weeks |

### 3.8 Licences and upstream

- X11R6.3: X Consortium licence (MIT-style). XFree86 3.3.6 parts used (`afb`, `ilbm`, `iplan2p*`, xkb data): MIT/X11-style. Apple `macII`: permissive notice (keep the copyright line where code is adapted). The AMIX port: MIT (`LICENSE`), imported files keep their notices.
- The Commodore `server/ddx/amix` on the AMIX tape is proprietary and not copied. The port carries its own forward-ported version with its notices; we change only our new files and the `rtg` table.
- New files (`rtg/fb`, `amixEv.c`, `amixMacKeyMap.c`, the `rtg` table): MIT. Offer them upstream as pull requests to `isoriano1968/x11r6.3-amix`: first the `rtg` table (no behaviour change for ZZ9000), then `rtg/fb` + event input + `-kb`-free keymaps. Propose a combined server target (backends chosen at run time) instead of one binary per card.

## 4. A/UX Mac environment on the service

uinter is an in-kernel session of kind `mac`.

| A/UX piece | On the service |
|---|---|
| Fake NuBus card, `UI_PHYS_SCREENS` | slot-space frame-buffer pages are the session's fb segment (VRAM in front, shadow hidden); the synthetic declaration ROM pages sit at the top of slot space. Its VPBlocks are generated at session creation from `FBIOGMODES` (base offset, row bytes, bounds, depth), so the Mac sees the real geometry |
| Slot Manager (syscall 66) | unchanged, over that image |
| `VIDEO_CONTROL` SetEntries / GetEntries / SetGray | `ds_setcmap`/`ds_getcmap` of the session (grey done in C) |
| SetMode / GetMode | `ds_setmode` (D5); before that only the current depth is offered |
| SetInterrupt, slot VBL | `ds` VBL hook; the kernel cursor (`c_cursor`, `c_lock`, `c_newcrsr`) is drawn at VBL only while the session is in front |
| `UI_DEVICES` → `ui_key_intr(adb_code, down)`, `ui_mouse_intr(dx, dy, btn)` | `ops->input`: ADB codes as they are; other hosts through `ds_kxlate` |
| `UI_KILLMYLAYER`, exit | `ds_close`: the next session comes to front |
| Exclusive sessions (`/mac/lib/sessiontypes`: console, mac24, mac32, X11) | no longer exclusive: the Mac desktop, X and the console run together and switch by hotkey |

### 4.1 X inside the Mac, and the Mac inside X (stretch)

- **MacX** (Apple's X server for the Mac OS, user-supplied): runs inside the Mac environment as any Mac application. No work beyond a test.
- **Rootless X in the Mac** (stretch, large): a rootless mode in the X server that backs each top-level window with a shared pixmap, and a Mac-side application that shows each as a Mac window and returns Mac events over a Unix socket. Model: the MIT-licensed rootless layer of later X servers, back-ported to R6.3. 6–10 weeks.
- **Mac in an X window** (cheaper, shares §5.3): a viewer X client maps the hidden Mac session's backing read-only (`FBIOVIEW`), converts written pages at ≤ 25 Hz, and sends X input with `EVIOCINJECT`.

## 5. Containers

guestsvc sessions ([guest-container-design.md](guest-container-design.md) §4.2, §10.3):

1. **Zero-copy** (guest format = host mode, e.g. Mac 1 bpp or TOS ST-high on a 1-bpp DAFB): the guest's surface pages are the session's fb segment.
2. **Converted** (P96 8-bit chunky, planar, other sizes): the surface is guest pages; guestsvc converts written pages into the session's view (VRAM in front, nothing while hidden), ≤ 50 Hz.
3. **Windowed**: an X client views the surface (`FBIOVIEW` on the guest's session) and injects input (`EVIOCINJECT`), the same path as §4.1.

Palette → `ds_setcmap`. Input: `ops->input`, then the profile's mapping (ADB, Amiga raw keys, IKBD) through `ds_kxlate`. The hotkey never reaches a guest. The "switch-out copies the screen and remaps the surface" rule of guest-container-design §10.3 is exactly the session switch here.

## 6. Amiga and Atari

Same `/dev/fb`, `/dev/kbd`, `/dev/mouse`, same X binary (AMIX ABI on every host).

| Host | Display backend | X screen code | Input |
|---|---|---|---|
| Amiga chipset | `FBL_PLANES` (bitplanes `fi_planebytes` apart), OCS/ECS/AGA colour registers as CLUT (`fi_cmapbits` 4 or 8), modes from AMIX's display types (hires, lace, A2024); VBL = AMIX level-3 servers | 1 plane: mfb; more: `afb` from XFree86 3.3.6. Stock AMIX kernels keep the port's `amixMono` path over `/dev/screen` | keyboard via CIA serial, mouse via `JOYxDAT`: `EVK_AMIGA`, the port's `amixKeyMap.c` |
| Amiga RTG (ZZ9000, VA2000, Picasso) | chunky, `FBL_PACKED` from the card driver; ZZ9000 keeps `/dev/zz9000` for its accelerated backend | `rtg/zz9000` or `rtg/fb` | as above |
| Atari ST/TT shifter | ST-high 640×400×1, TT-high 1280×960×1 (`FBL_PACKED`, mfb); TT-medium 640×480×4, TT-low 320×480×8 (`FBL_IPLAN2`) | mfb; `iplan2p4`/`iplan2p8` from XFree86 3.3.6 | IKBD scancodes (`EVK_IKBD`, XKB `ataritt` + 8), IKBD relative mouse |
| Falcon Videl | 16 bpp TrueColor (`cfb16`), 8-bit interleaved (`iplan2p8`) | | as above |

ASV's `/dev/video` is not reused: its layout is unknown and ASV's ABI differs from AMIX's; the Atari backend is written for the unified kernel. The planar layers (`afb`, `ilbm`, `iplan2p*`) are R6.3-era MIT code and drop into the port's tree.

## 7. Stages (QEMU `q800`)

Tests drive QEMU through its monitor: `screendump` (compare PPMs), `sendkey`, `mouse_move`, `mouse_button`.

| Stage | Content | Pass criteria |
|---|---|---|
| **D1** | `ds` core, `/dev/fb0` (info, modes, CLUT, `mmap` WT/CI, notes), `/dev/kbd`, `/dev/mouse`, sessions, console fallback; `dstest` tool (cross-built) | `dstest` fills a pattern at the boot depth and cycles the CLUT at 8 bpp: screendumps match; injected keys and moves print with rising timestamps and correct ADB codes; `kill -9 dstest` → console in front with its text; two `dstest` sessions switch by hotkey with contents intact; a background `dstest` sees no input |
| **D2** | server: `rtg` table, `rtg/fb` depth 1, `amixEv.c`, `amixMacKeyMap.c`, cross build; clients from the port | at 1 bpp: `xdpyinfo` (StaticGray, 1 plane), `twm` + `xterm` + `xclock`; a shell in xterm; Shift/Control/Option/Command right; `-mb3` emulation; server exit (twm Exit, and `kill -9`) returns the console |
| **D3** | 8 bpp PseudoColor, 16 bpp, 32 bpp (QEMU `-g …x8`, `x24`) | `xcmap`/`ico -colors` change the CLUT; installing another colormap works; the console CLUT is back after exit; WT vs CI measured with `x11perf -copywinwin500 -rect500` |
| **D4** | switching: console, X, a second X or `dstest`, the Mac environment | hotkey cycles all with contents and CLUTs intact; `xclock` advances while hidden; no stuck modifiers; owner crash and the emergency key return the console; a panic shows its message |
| D5 | DAFB mode set | `X -depth 8` from a 1-bpp A/UX Startup boot; each session gets its own depth back after a switch; then on a Q800 |
| D6 | upstream and native build | PRs accepted or rebased; native server build in QEMU reproduces the cross build's behaviour |
| D7 | Amiga backends (chipset planar, RTG through `/dev/fb`) | same X binary: twm + xterm on an Amiga under our kernel |
| D8 | Atari backends (shifter, Videl, IKBD) | same on TT/Falcon |
| S1 | Mac in an X window (`FBIOVIEW`, `EVIOCINJECT`) | Finder in an X window, usable at 1 bpp |
| S2 | rootless X in the Mac | xterm as a Mac window |

## 8. Open questions

1. DAFB: blank register, mode-set registers and DAC depth modes (D5, from the ROM); VBL delivery in QEMU.
2. Q800 VRAM size and its mode table (needs a Q800 ROM).
3. Hotkey defaults vs Mac applications and keyboards without F-keys.
4. Shadow memory: locked kernel pages (simple) or pageable anon (large modes, many sessions).
5. `/dev/conslog` on AMIX: present in stock `X`'s strings; check its driver in the kit.
6. Whether amigaux.org's planned X11R6.3 packages are this port; align `ProjectRoot` (`/usr/x11r6`).
