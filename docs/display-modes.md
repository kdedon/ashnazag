# Guest-requested display modes

Replaces "environments take the host screen size" on hosts whose display has several modes. The host lists its modes; a session asks for one; the hardware shows each session's own mode while that session is in front. No guest ever programs the hardware itself.

## What exists

- `FBIOGMODES`/`FBIOSMODE`, a mode per session (`s_vmode`), applied at switch-in, `FBN_MODE` notes.
- Atari Videl: real resolution modes per session (`ad_vm`, `ata_vmset`, `ata_vsave`/`ata_vload` with `ds_vcons` for the console).
- Amiga RTG: depths only, at the one size `Space.c` names; each card has a VESA timing table (Voodoo3, Radeon) or firmware mode numbers (ZZ9000) but sets one at load.
- Mac: `fbprobe` lists declaration-ROM depths of one resolution; `FBIOSMODE` is `ENXIO`.
- Card memory (#214): the front frame at 0, hidden sessions in RAM or an own frame, 3D above.

## Mode list

One table per display, built by the backend at attach:

```
struct fbmodeinfo {        /* grows; FBIOGMODES unchanged otherwise */
	mi_id, mi_width, mi_height, mi_depth, mi_rowbytes, mi_offset;
	mi_flags;              /* FBM_CURRENT, FBM_SCALE2 (card doubles it), FBM_TRIED */
	mi_hz;
};
```

A mode is a (timing, depth) pair. Backends supply timings; the list is the card's table cut to what the monitor takes where the card reads EDID, else the whole table: the trial protects against modes the monitor can't show.

| Host | Modes |
|---|---|
| Mac DAFB, NuBus | one resolution, its depths (list of one size) |
| Amiga ECS | today's 640-wide plane modes |
| Voodoo3, Radeon | VESA table: 640x480 to 1280x1024, 8/16/32 |
| ZZ9000 | firmware modes 640x400 to 2560x1440; `SCALE2` variants (card doubles a half-size frame) |
| Falcon Videl | `ad_vm` (VGA monitor) |
| SuperVidel | `sv=` table, 8/16/32; Videl modes stay refused while it drives the screen |
| TT | its fixed shifter modes |

`am_rtg` gains `r_modes`, `r_nmode` and `r_setmode(n, bits)`; `r_setdepth` becomes the depth-only case. `r_setmode` must run at high IPL without sleeping (panic path).

## Request and grant

New `FBIOREQMODE`, in/out:

```
struct fbreq {
	rq_width, rq_height, rq_depth;   /* in: what the guest wants */
	rq_flags;                        /* in: FBR_EXACT, FBR_NOSCALE */
	rq_id;                           /* out: mode granted */
	rq_x, rq_y;                      /* out: guest screen's origin in it */
	rq_how;                          /* out: FBG_EXACT, FBG_CENTRE, FBG_SCALE, FBG_CLIP */
};
```

Grant, first match:
1. Exact size and depth listed: that mode.
2. A `SCALE2` mode at twice the size (ZZ9000), unless `FBR_NOSCALE`.
3. The smallest listed mode at that depth that holds the request: the guest screen centred in it. `FBIOGINFO` then reports the requested width and height, the mode's `fi_rowbytes`, and `fi_offset` at the centred first pixel, so guests that honour rowbytes need no change.
4. Larger than every mode: the largest, clipped; the environment pans or scales as it does now.

`FBR_EXACT` turns 2-4 into `ENXIO`. `FBIOSMODE` stays as the by-id form.

## Per-session state and switching

Each session holds its mode index and origin. The console holds its own, set at boot and never changed by a guest.

- Request while hidden: recorded, note posted, no hardware touched.
- Request while in front: the same steps as a switch, to itself.
- Switch (`ds_switch`), when the modes differ:

```
blank DAC -> unload both mappings -> evict from's frame (#214)
  -> r_setmode(to) in the vertical blank -> relayout: to's frame at 0, sized
  for its mode -> copy in / pan -> CLUT -> 3D fronthook (pitch, size) -> unblank
```

Same mode: today's path, no resync. A monitor resync (about a second) is the cost of a mode change; it never happens on a same-mode switch.

Sizing: a session's mapping is sized for the largest listed mode at its depth when acquired (`FBIOVSIZE` grows it later); shadow pages are allocated on touch and card memory only in front, so the size costs nothing until used, and a mode change never needs an unmap.

Card memory: frames are mode-sized, not `d_vsize`. `am_vneed`, `am_vslot` and `am_vheld` take each session's frame size. A mode set that leaves no room for an own-frame session evicts it to RAM first; the 3D driver's buffers belong to their session and are evicted with it.

## Never stranded

- The console hotkey always works and restores the console's mode: no confirmation needed to get out.
- Panic and `fbcons_panicfn` load the console mode through `r_setmode`.
- An unconfirmed mode is on trial: 15 s, with a one-line prompt drawn by the console renderer and redrawn each second over whatever the guest draws there. Return (taken in the input path, as hotkeys are) or `FBIOMODEOK` from the session (sent when the guest's own "keep?" dialog is accepted) keeps it and marks it `FBM_TRIED`; otherwise the best confirmed mode holding the session's screen returns, the picture moved into its place, and the session gets `FBN_MODE`. One trial at a time; other requests get `EBUSY`.
- Confirmed modes persist per environment, in its folder, with the monitor's identity (EDID, else the card's). The environment's starter passes them in as root (`FBIOMODEOK` with the id, before the request); a changed monitor makes them unconfirmed, so they go through the trial again.

## Security

- `FBIOREQMODE`/`FBIOSMODE`: the session's owner or root (`ds_owns`), as now.
- Hardware is programmed only for the session in front, from `ds_switch` or a front session's own request. A hidden session changes only its record.
- Only listed modes: ids index the backend's table; no caller-supplied timings or PLL values.

## Environments

| Environment | Native call | Maps to |
|---|---|---|
| Amiga (P96) | card driver's mode list; `SetGC`/`SetSwitch` on `OpenScreen`, `BestModeID` | list from `FBIOGMODES` per format; `SetGC` asks via a host call to `FBIOREQMODE`; `BestModeID` picks the nearest listed. "Smaller modes centred" becomes grant 3 when the card lacks the size |
| TOS (EmuTOS) | XBIOS `Setscreen` rez/mode, `Esetshift` (hooked) | host call after the original asks for the ST/TT size (TT high: 1280x960 when listed, else today's halving) |
| TOS on an Atari host | screen set-up | `FBIOSMODE`: the deepest bitplane Videl mode, as now |
| Mac | Monitors / Display Manager: Control `cscSwitchMode`, Status `cscGetNextResolution`, `cscGetModeTiming`, `cscGetVideoParameters` | declaration ROM gets one resolution sResource per listed size; `cscSwitchMode` asks; the Display Manager's own "keep?" dialog sends `FBIOMODEOK`. Mac host: one entry, as now |
| X | server start `-mode WxHxD` | `FBIOREQMODE` once; VidMode at run time later, optional |
| NeXT, Lisa, V200, Win16 | fixed size | default: the host's size, centred; a per-environment setting asks at start for the nearest mode that holds it |

## Work plan

| # | Run | Size |
|---|---|---|
| 1 | ds core: mode table, `FBIOREQMODE` grant, origin via `fi_offset`, switch path, trial/confirm, per-environment confirmed modes, console restore; Mac and Atari Videl backends on it; `t_display` cases | 2 runs |
| 2 | `am_rtg` modes; Voodoo3 and Radeon `r_setmode` from their tables; mode-sized card-memory frames; 3D fronthook pitch. Amiberry Voodoo3 | 1 run |
| 3 | ZZ9000 firmware modes and `SCALE2` | 1 run (friend hardware) |
| 4 | SuperVidel mode set at switch time | 1 run (friend 060) |
| 5 | Amiga environment: P96 list, `SetGC` request, `BestModeID` | 1 run |
| 6 | TOS `Setscreen` request, TT high when listed | 1 run |
| 7 | Mac declaration ROM resolutions, Display Manager csCodes, tested on the Amiga host | 1-2 runs |
| 8 | X `-mode`; fixed-size environments' start setting; README/environments.md | 1 run |

About 9-10 runs; 1 gates the rest, 2-4 and 6 can run in parallel after it, 5 after 2.
