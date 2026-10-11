# 3D: Radeon R200, Mesa 5.0 and Warp3D

Accelerated GL on an R200 (Radeon 8500/9000/9200) behind an Amiga PCI bridge, and a Warp3D.library in the Amiga environment that calls Mesa in its own process. Software Mesa everywhere else. Sizes are lines of C/assembly counted in the public sources named below.

Sources (all MIT/BSD-style):

| Source | What we take |
|---|---|
| XFree86 4.3.0 (`X430src-1..3`) | DRM kernel code (`os-support/shared/drm/kernel`, `os-support/bsd/drm/kernel`), libdrm (`os-support/linux/drm/xf86drm*.c`), DRI drivers `r200`, `radeon` (`lib/GL/mesa/src/drv`), libGL client (`lib/GL/glx`, `lib/GL/dri`), server GLX/DRI (`programs/Xserver/GL`), radeon 2D driver, x86emu |
| Mesa 5.0.2 | GL core, swrast, tnl, OSMesa, Xlib driver, GLU |
| Warp3D 4.2a SDK (Aminet `dev/misc/Warp3DDev-4.2a`) | `Warp3D.h`, `Warp3D.fd`: 85 LVOs |
| Wazp3D (Aminet `driver/video/Wazp3D`) | reference for a library-level replacement and its Warp3D-to-GL path |

Mesa 5.0 itself carries no DRI drivers: they live in the XFree86 4.3 tree, built against its Mesa 5.0 snapshot (`xc/extras/Mesa`, 342 files differ from 5.0.2). Use Mesa 5.0.2 for the core and the 4.3 drivers on top; fix the few API drifts as they show.

## 0. Summary

| Question | Answer |
|---|---|
| DRM | BSD flavour of the 4.3 DRM (OS macros, no Linux-isms) + shared radeon files, ported to a `drm_os_amix.h`. ~14k lines, ~1k new. PCI GART only, on memory from `opci_dmaalloc` |
| Card owner | the display service. A new Radeon backend (POST, modes, VRAM per session) holds the DRM; the kernel does the CP init that the X driver did on XFree86 |
| X server | **keep Xamix (X11R6.3)** and add direct-only GLX + the DRI extension: ~7k lines ported, ~1k new. XFree86 4.3's server is ~300k lines plus a new os-support port and a second owner for the card |
| Mesa | 5.0.2 core static, no threads, no dlopen at first. OSMesa on AMIX is the first build |
| Warp3D | our Warp3D.library (85 LVOs), register glue to ELF functions in startmig, Mesa behind it. Trap #0 already reaches AMIX from guest code; add a PC-range check so faults and FP emulation in Mesa go to the host, not the guest |
| Output | the Amiga environment's display-service session; on the card the session's VRAM, else OSMesa into the session memory |
| Card-free work | compiler, Mesa software, libGL/GLX, Warp3D.library on swrast, DRM core, opci DMA/IRQ (Amiberry). Card needed: Radeon backend, radeon DRM, r200 |

```
 Amiga environment (one AMIX process: startmig + guest)        X clients
   game / MiniGL / StormMesa                                    glxgears
   Warp3D.library (hunk) --register glue--> w3d_*() (ELF)      libGL + r200 (direct)
                                             Mesa 5.0 + r200       |
                                             libdrm               Xamix + GLX/DRI ext
                 trap #0: ioctl, mmap               \               /
 -------------------------------------------------------------------------- AMIX
   drm (DLM, $depend opci)  <-->  display service Radeon backend (modes, sessions)
        PCI GART on opci_dmaalloc memory, IRQ via opci_intr, MMIO/FB via BARs
 --------------------------------------------------------------------------
   openpci.library on amilib  ->  Mediator / Prometheus / G-REX  ->  R200
```

## 1. Radeon DRM

### 1.1 Which code

| Part | Files | Lines |
|---|---|---|
| DRM core templates (BSD) | `drmP.h`, `drm.h`, `drm_drv.h`, `drm_fops.h`, `drm_ioctl.h`, `drm_lock.h`, `drm_auth.h`, `drm_bufs.h`, `drm_context.h`, `drm_dma.h`, `drm_drawable.h`, `drm_memory.h`, `drm_scatter.h`, `drm_vm.h`, `drm_lists.h`, `drm_init.h`, `ati_pcigart.h` | 7,000 |
| Not taken | `drm_agpsupport.h`, `drm_sysctl.h`, `drm_os_freebsd.h`, `drm_os_netbsd.h` | (1,700) |
| Radeon | `radeon_cp.c`, `radeon_state.c`, `radeon_irq.c`, `radeon_mem.c`, `radeon_drv.h`, `radeon_drm.h`, `radeon.h`, `radeon_drv.c` | 6,200 |
| New | `drm_os_amix.h` (macros, ~400), AMIX glue (cdevsw, ioctl copy, mmap, waits, opci attach, ~600) | ~1,000 |
| libdrm (user) | `xf86drm.c`, `xf86drmHash.c`, `xf86drmRandom.c`, `xf86drmSL.c`; drop `/proc`, mknod, Linux device naming | 3,700, ~200 changed |

The BSD templates are chosen over the Linux 2.4 ones: everything OS-specific is already behind `DRM_*` macros (`DRM_READ32`, `DRM_WAIT_ON`, `DRM_COPY_FROM_USER_IOCTL`, `DRM_MALLOC`, `DRM_VTOPHYS` ...), so the port is one header plus glue. Linux 2.4 DRM is the same code with Linux calls inline. Later fixes from DRI CVS (2003-04) for big-endian and PCI R200 are taken as patches when the card shows the need.

### 1.2 What it needs from the OS, and what we have

| DRM need | 4.3 BSD form | AMIX / ours |
|---|---|---|
| Find the card, config space | `pci_get_*`, `pci_read_config` | `opci_find(0, 0x1002, id)`, `opci_cfgread/write`, `opci_obtain` |
| MMIO registers (BAR2, 64 KB) | `bus_space`, `DRM_READ32` | `opci_attr(OPCI_BARADDR(2))`: already a kernel VA, uncached and serialized; `DRM_READ32/WRITE32` byte-swap (§1.4) |
| Framebuffer (BAR0, 64-128 MB) | map by physical address | `OPCI_BARADDR(0)`; user mappings by `d_mmap` returning `phystopfn` (as `zz9000mmap`). Prometheus/Mediator Z4 map whole BARs; Mediator A1200 shows them through its fault window (one fault per bank change, slow but correct) |
| AGP | `agp_*` | **none**: `__HAVE_AGP 0`, CP init with `is_pci = 1` (the 4.3 PCI path) |
| PCI GART | `drm_scatter.h` allocs pages, `ati_pcigart.h` writes the table | table and pages from `opci_dmaalloc` (memory the card can master into), bus addresses from `opci_busaddr`, not `vtophys` |
| Ring, IBs, vertex buffers | in the SG area | same SG area (ring 1 MB, buffers 2 MB is enough) |
| Shared memory (SAREA, 4-8 KB) | `DRM_MALLOC` + map | `kmem_alloc` page-aligned; `d_mmap` gives its pfn (`plat_vtop`) |
| Interrupts (vblank, SW int for fences) | `bus_setup_intr` | `opci_intr`: handler at the bridge's Zorro level 2/6. Optional at first: r200 polls when `RADEON_PARAM_IRQ_NR` is 0 |
| Sleep with timeout | `tsleep(PCATCH, timo)` | `sleep(chan, PZERO+1|PCATCH)` + `timeout()`/`untimeout()`; EINTR is fine, libdrm retries |
| `DRM_UDELAY` | `DELAY` | `amx_delayus` / `delayus` |
| Spinlocks, device lock | `mtx`/`lockmgr` | `spl` from the bridge level (`amx_spl`), single CPU |
| ioctl argument copy | the BSD kernel copies by `_IOC_SIZE` | AMIX drivers copy themselves: the glue decodes the Linux-style size and direction in `cmd`, `copyin` before and `copyout` after the call |
| mmap of maps (regs, FB, SAREA, SG, buffers) | `d_mmap` | `d_mmap` + `spec_segmap`, as the zz9000 module |
| Per-open state, auth | `drm_file`, magic via X | one `drm_file` per open; authentication by session: `DRMIOC_DSBIND(fbfd)` ties the fd to the caller's own display session (as `EVIOCBIND`). No X server needed as master |
| Module form | kld | DLM device module `radeon` (`$depend opci`), autoloaded on first open of `/dev/dri/card0` |

Who does `radeon_dri.c`'s job (CP init, map creation, heaps, SAREA layout; 1,860 lines in the X driver): the kernel, at first open, from the backend's VRAM layout (front, back, depth, texture heap per session). Clients only `GETPARAM`, map, lock and submit. That keeps the card single-owner (display service) and lets the Amiga environment use GL without X.

### 1.3 Bus mastering on Amiga bridges

The DRM needs the card to read host memory (ring, IBs, vertices, GART table) and write a little (read-pointer and scratch writeback; `writeback_works` is tested and can stay off).

| Bridge | Card to Amiga RAM | Plan |
|---|---|---|
| G-REX | yes, CPU RAM | PCI GART over `opci_dmaalloc` memory |
| Mediator Z4 / A1200 | through the bridge's DMA window (a part of Fast RAM openpci configures) | same; size of the window limits SG to a few MB |
| Prometheus | believed not supported by the board | to verify on the user's bridge with `opci_dmaalloc`; fallback below |

Fallback without bus mastering: ring and buffers in VRAM (the CP fetches through the card's memory controller) or CP PIO mode. Neither is in the 4.3 DRM; both are a risk to check on the real card before relying on them. No PCI driver has used `opci_dmaalloc` or `opci_intr` yet: test those first (§5, Amiberry).

### 1.4 Big-endian spots

PowerPC Linux ran this code because `readl`/`writel` swap and a few `#ifdef __BIG_ENDIAN` blocks set the card's swappers. On AMIX nothing swaps by itself: the bridges pass PCI bytes address-invariant.

| Where | 4.3 status | Our action |
|---|---|---|
| MMIO `DRM_READ32/WRITE32` (`radeon_drv.h` RADEON_READ/WRITE) | BSD macros: plain loads | swap in the macro (`ror.w #8; swap; ror.w #8`); 8-bit accesses unchanged |
| Ring head and scratch writeback (`GET_RING_HEAD`, `scratch[]`, read with `DRM_READ32`) | swapped on PPC via `readl` | same swapping macro covers it |
| Ring fetch | `RADEON_BUF_SWAP_32BIT` in `CP_RB_CNTL` under `__BIG_ENDIAN` (`radeon_cp.c:955`) | define `__BIG_ENDIAN` in `drm_os_amix.h` |
| Host-data blits (texture upload) | `RBBM_GUICNTL = HOST_DATA_SWAP_32BIT` under `__BIG_ENDIAN` (`radeon_state.c:1091`) | as is for 32-bit texels; 16-bit texels need the half-word swap: check, take the later DRI fix if wrong |
| PCI GART entries | `cpu_to_le32` (`ati_pcigart.h:118`), identity on BSD | real swap |
| Vertex fetch | `R200_VC_32BIT_SWAP` in `SE_VAP_CNTL_STATUS` (`r200_state_init.c:440`) | set `MESA_BIG_ENDIAN` |
| 16-bit element indices | `EMIT_ELT` reorder (`r200_tcl.c:215`, `r200_swtcl.c:602`) | same |
| CPU access to VRAM (spans, software fallbacks, Workbench, Xamix cfb) | `SURFACE_CNTL` `NONSURF_AP0_SWP_16BPP/32BPP` set by the X driver (`radeon_driver.c:5020`) | the Radeon backend sets it per session depth |
| 2D engine host data | `DP_DATATYPE HOST_BIG_ENDIAN_EN` (`radeon_accel.c:284`) | only if we add 2D acceleration |
| Mesa `glheader.h` | `MESA_BIG_ENDIAN` only if `BYTE_ORDER == BIG_ENDIAN`; `CPU_TO_LE32` identity outside Linux ("fix me") | `-DBYTE_ORDER=4321 -DBIG_ENDIAN=4321`, real `CPU_TO_LE32` |
| DRM fast lock `DRM_CAS` (`xf86drm.h`) | no m68k case: falls back to "lock fails", one ioctl per lock | add `casl` (68020-060 in hardware) |

## 2. X server: DRI in X11R6.3 or XFree86 4.3

| | A: Xamix (X11R6.3) + GLX/DRI | B: XFree86 4.3 server |
|---|---|---|
| Server core | kept (dix 28k, os 11k, mi 29k, cfb 36k, Xext 19k, amix/rtg 7k) | new: dix 30k, os 14k, mi 36k, fb 20k, Xext 35k, render 8k |
| XFree86 layer | stubs for the few `xf86*` calls DRI makes (messages, `xf86Screens`, loader checks) | common 41k, os-support 37k (a new AMIX/SVR4 port), loader 13k, parser 7k, xaa 24k, ramdac/vgahw/ddc/int10 11k |
| Card driver | none: the display service owns modes; Xamix keeps its `fb` card | radeon 2D 13k (+ati wrapper), and it wants to own modes: conflicts with the display service |
| GLX (server) | direct-only: `glxcmds`, `glxext`, `glxscreens`, `glxutil`, `global`, `glximports` 3.9k | full GLX 29k + Mesa core again for indirect rendering |
| DRI extension | `dri.c`, `xf86dri.c` 2.9k; R6.3 `ScreenRec` already has the `ValidateTree`, `PostValidateTree`, `ClipNotify`, `WindowExposures`, `CopyWindow` hooks it wraps | 3.5k |
| DDX DRI hooks | ~0.5k new in the `fb` card: lock + wait-idle around cfb access, cliprects, buffer offsets from the backend | `radeon_dri.c` 1.9k |
| Lost | nothing | the RTG card table, ZZ9000/ViRGE/planar cards, display-service input, `iplan2p8` (re-port as 4.3 drivers) |
| Total to port | **~7k ported, ~1k new** | **~300k ported, ~5k new** |

Recommendation: **A**. X is one client of the display service like the Amiga environment; the card stays single-owner; indirect GLX (remote clients) is not needed on these machines. Client side for both: X11R6.3 libX11/libXext (the existing `clibs` stage) and 4.3's libGL (`glx` 20k with the indirect protocol encoder, `dri` 2.2k).

## 3. Mesa 5.0.x on AMIX

### 3.1 Pieces

| Library | Contents | Lines |
|---|---|---|
| Mesa core | `src/*.c` 74k, math 7k, array_cache 1.5k, swrast 25k, swrast_setup 1.5k, tnl 15k, tnl_dd 5k | 129k |
| OSMesa | `src/OSmesa/osmesa.c` | 2.2k |
| Xlib driver (software libGL) | `src/X` | 16k |
| GLU | `si-glu` | 23k |
| r200 DRI | `drv/r200` + `drv/common` | 21k |
| radeon (R100) DRI | `drv/radeon` | 17k |
| libGL with DRI | `lib/GL/glx`, `lib/GL/dri` (`dri_glx.c` loader, `dri_util.c`, `XF86dri.c`) | 22k |

### 3.2 AMIX facts that shape the build

| Fact | Consequence |
|---|---|
| `libdl.so` with `dlopen(RTLD_LAZY/RTLD_NOW)`, no `RTLD_GLOBAL` | the loader works as is with `RTLD_GLOBAL` defined 0: each `*_dri.so` carries its own Mesa core and needs only `_glapi_*` from libGL, which the executable's scope provides. First builds link `r200` statically into libGL / the Warp3D host: no `.so` building with binutils 2.8.1 needed until the new toolchain is in |
| No threads | build without `THREADS`; `glthread.c` uses its single-thread path |
| No `snprintf` | Mesa's `vsnprintf.c`; 2 `snprintf` in `dri_glx.c` |
| `libm.a` only | static libm; Mesa uses double functions only |
| Big-endian | `-DBYTE_ORDER=4321 -DBIG_ENDIAN=4321` (§1.4) |
| FPU | 040/060 emulate `fsin` etc. in the kernel FPSP; FPU-less 030 Atari needs a `-msoft-float` build |

### 3.3 First build: software only

1. `libOSMesa.a`: core + swrast + tnl + OSMesa, with gcc 2.95.3 (2.7.2.3 as a check). Test: `osdemo` writes a PPM; compare with the same demo on the host.
2. `libGL.so`/`libGL.a` from `src/X` (Xlib driver) against X11R6.3 client libs; `glxgears` under Xamix. The server needs no GLX for this: the Xlib driver draws with core X requests.
3. GLU and the MesaDemos as a package.

## 4. Warp3D.library

### 4.1 The API

Warp3D V4 (68k, 4.2a) is 85 LVOs from -30 (`Warp3D.fd`). V5 is the AmigaOS 4 PPC API (Hans de Ruiter's Radeon drivers) and not used by 68k titles. Tiers below come from MiniGL 68k, StormMesa's driver, Wazp3D's notes and its known-buggy-call list; confirm them with the library's own per-call counters on real titles (§5 step 4).

Warp3D vertices are already in window space: x, y in pixels, z in 0..1 (double), w = 1/z_eye for perspective, u, v in texels unless normalised. The host side draws them with identity modelview, `glOrtho(0, w, h, 0, 0, -1)`, and texture coordinates `(u·w, v·w, 0, w)` scaled by the texture matrix: the GL path Wazp3D uses.

| Tier | Calls | GL / host mapping |
|---|---|---|
| Core (all users) | `CreateContext`, `DestroyContext` | DRI context + drawable on the environment's session (or OSMesa context) |
| | `SetState`, `GetState` | `W3D_TEXMAPPING` → `GL_TEXTURE_2D`; `GOURAUD` → `glShadeModel`; `ZBUFFER` → `GL_DEPTH_TEST`; `ZBUFFERUPDATE` → `glDepthMask`; `BLENDING`, `FOGGING`, `DITHERING`, `SCISSOR`, `ALPHATEST`, `STENCILBUFFER`, `LOGICOP`, `CULLFACE`, `ANTI_*` → the matching `glEnable`; `PERSPECTIVE` → use w; `INDIRECT`, `AUTOTEXMANAGEMENT`, `SYNCHRONIZATION`, `FAST` → host-side flags |
| | `LockHardware`, `UnLockHardware` | exec semaphore; `W3D_NOTVISIBLE` while the session is hidden. Mesa takes the DRM lock itself per flush |
| | `AllocTexObj`, `FreeTexObj`, `UpdateTexImage`, `UpdateTexSubImage`, `UploadTexture`, `ReleaseTexture`, `FlushTextures`, `FreeAllTexObj` | `glGenTextures`/`glTexImage2D`/`glTexSubImage2D`/`glDeleteTextures`; upload and release are no-ops (Mesa manages VRAM) |
| | `SetFilter`, `SetWrapMode`, `SetTexEnv` | `glTexParameteri` (nearest/linear/mipmap), `GL_REPEAT`/`GL_CLAMP`, `glTexEnvi` replace/decal/modulate/blend |
| | `SetBlendMode`, `SetAlphaMode`, `SetZCompareMode`, `SetFogParams`, `SetColorMask`, `SetCurrentColor`, `SetScissor` | `glBlendFunc`, `glAlphaFunc`, `glDepthFunc`, `glFog*` (interpolated fog via `EXT_fog_coord`), `glColorMask`, `glColor4f`, `glScissor` (y flipped) |
| | `AllocZBuffer`, `FreeZBuffer`, `ClearZBuffer`, `ClearDrawRegion` | depth buffer of the drawable (VRAM from the backend); `glClear` with `glClearDepth`/`glClearColor` |
| | `SetDrawRegion` | drawable's colour offset and pitch = the BitMap's place in the session (§4.4); `glViewport` |
| | `DrawTriangle`, `DrawTriFan`, `DrawTriStrip` and `...V` | `glBegin`/`glEnd` or a converted vertex array (`GL_TRIANGLES`/`FAN`/`STRIP`) |
| | `Flush`, `FlushFrame`, `WaitIdle`, `CheckIdle` | `glFlush`, `glFlush`, `glFinish`, idle after the last fence |
| | `CheckDriver`, `Query`, `GetTexFmtInfo`, `GetDestFmt`, `GetDriverState`, `Hint` | answered by the library (one "driver": R200 or CPU); `glHint` |
| | `RequestMode`, `BestModeID`, `TestMode`, `GetScreenmodeList`, `FreeScreenmodeList`, `GetDrivers`, `QueryDriver`, `GetDriverTexFmtInfo` | from the environment's P96 modes; no GL |
| V4 arrays (MiniGL 1.2+, BlitzQuake) | `VertexPointer`, `TexCoordPointer`, `ColorPointer`, `BindTexture`, `DrawArray`, `DrawElements`, `SetFrontFace` | `glVertexPointer` (`W3D_VERTEX_F_F_D` has a double z: converted to float), `glTexCoordPointer`, `glColorPointer` (ARGB order converted), `glBindTexture`, `glDrawArrays`/`glDrawElements`, `glFrontFace` |
| Rare | `DrawLine`, `DrawLineStrip`, `DrawLineLoop`, `DrawPoint` | `GL_LINES`/`LINE_STRIP`/`LINE_LOOP`/`POINTS`, `glLineWidth`/`glPointSize` |
| | `AllocStencilBuffer`, `FreeStencilBuffer`, `ClearStencilBuffer`, `FillStencilBuffer`, `SetStencilFunc`, `SetStencilOp`, `SetWriteMask`, `Read/WriteStencilPixel/Span` | Z24S8 buffer; `glStencilFunc/Op/Mask`, `glClear`, `glRead/DrawPixels(GL_STENCIL_INDEX)` |
| | `ReadZPixel`, `ReadZSpan`, `WriteZPixel`, `WriteZSpan` | `glReadPixels`/`glDrawPixels(GL_DEPTH_COMPONENT)` (masked spans per run) |
| | `SetLogicOp`, `SetChromaTestBounds` | `glLogicOp`; chroma test → alpha at upload + `glAlphaFunc` |
| CLUT screens only | `SetCurrentPen`, `SetPenMask` | `W3D_UNSUPPORTED` on R200; software path only |
| | `SetDrawRegionWBM` | drawable on a plain memory bitmap: OSMesa |

Texture formats (Warp3D bytes are big-endian; GL packed types are in host order, so no swapping on our side):

| Warp3D | GL format / type |
|---|---|
| `A1R5G5B5` | `GL_BGRA` / `GL_UNSIGNED_SHORT_1_5_5_5_REV` |
| `R5G6B5` | `GL_RGB` / `GL_UNSIGNED_SHORT_5_6_5` |
| `A4R4G4B4` | `GL_BGRA` / `GL_UNSIGNED_SHORT_4_4_4_4_REV` |
| `A8R8G8B8` | `GL_BGRA` / `GL_UNSIGNED_INT_8_8_8_8_REV` |
| `R8G8B8`, `R8G8B8A8` | `GL_RGB`, `GL_RGBA` / `GL_UNSIGNED_BYTE` |
| `A8`, `L8`, `L8A8`, `I8` | `GL_ALPHA`, `GL_LUMINANCE`, `GL_LUMINANCE_ALPHA`, `GL_INTENSITY` |
| `CHUNKY` + palette | expanded to ARGB by the library (R200 has no palettes) |

### 4.2 Calling Mesa from guest code

```
 Amiga task (user mode)                        same process, ELF side (startmig)
   jsr -LVO(a6)
   Warp3D.library stub: args d0-d4/a0-a4 -> stack (as bsdsocket.library's lvo macro)
     ObtainSemaphore(lib lock)        one caller in Mesa at a time
     usp -> Mesa stack (256 KB)        task stacks are 4-16 KB
     save FPCR, set GL's
     jsr host->fn[n] ----------------------->  w3d_xxx(args...)  -> Mesa
                                                   flush: ioctl(drm, CMDBUF)  trap #0 -> AMIX
     <---------------------------------------  d0 (pointers also in a0)
     restore FPCR, usp; ReleaseSemaphore; rts
```

- **Finding the ELF side**: startmig writes a table `{magic, version, count, fn[85]}` at a fixed guest address, as it does for the RTG share (`MIG_RTG_BASE`). Warp3D.library checks magic and version at open; without them `OpenLibrary` fails and titles fall back as on a machine without 3D.
- **ABI**: AmigaOS and SVR4 m68k agree on scratch registers (d0-d1/a0-a1, fp0-fp1) and saved ones (d2-d7/a2-a6, fp2-fp7). Arguments are copied to the stack in C order. Floats only travel in structures. Pointer results go back in d0 and a0.
- **Reentrancy**: Mesa, libc malloc and libdrm are not reentrant. The semaphore covers them. A task switch inside Mesa (a guest interrupt, then `Schedule`) is safe: the next Warp3D caller waits on the semaphore; exec saves the usp that points into the Mesa stack.
- **Memory**: Mesa's `malloc` uses the process's `brk`, above startmig's data and clear of the guest's fixed ranges. Textures and vertex arrays are read in place in guest memory: same address space, same byte order.
- **Blocking**: an ioctl that sleeps (lock contention, wait idle) stops the whole environment. Waits are short (a frame at most) and interruptible: a host signal ends the sleep with `EINTR`, the guest interrupt is delivered on the way out, libdrm retries.

### 4.3 Mesa's system calls and exceptions

What the guest path does today (`guestcore`, `amigaguest`, `mkgates.py`):

| Vector | Guest handling now | From Mesa code we need |
|---|---|---|
| 32, trap #0 | never reflected: `mkgates.py` gives the fast trap gate only to traps #1-#15; `amigaguest_load` leaves `gpf_disp[32]` empty, so `gc_trap` declines to the stock syscall path. bsdsocket.library already calls AMIX this way | **works as is**: DRM ioctls, `mmap`, `brk`, `poll` reach AMIX |
| 33-47, traps #1-#15 | fast reflection (`GPF_FTRAP`) | not used by gcc output |
| 2, access fault | `amiga_fault` → guest bus error | host: `SIGSEGV` and a core, not a Guru |
| 11 F-line (040/060 FPSP: `fsin`, `fetox` ...), 55/60 unimplemented data/EA | `amiga_fline` / `amiga_refl` → the guest's 68040/060.library | host FPSP: guest libraries may be absent and are slower |
| 61, unimplemented integer (060: 64-bit `mul`/`div`) | reflected to the guest | host `isp` |
| 8, 10 | fast paths for guest code | not produced by gcc |

Change (~25 lines + one ioctl): a host PC range per guest process.

- `struct guest_proc` gets `gp_hostlo`, `gp_hosthi`; `AMIGAIOC_HOSTPC {lo, hi}` before `AMIGAIOC_ENTER`, set by startmig from its own text and data ends (and any later `dlopen`ed object).
- `gc_trap`: `if ((u_long)GR_PC(r) - gp->gp_hostlo < gp->gp_hosthi - gp->gp_hostlo) return 1;` before the profile dispatch. Declining sends the exception to the stock handler (`guest_chain`), exactly as for a native process. The asm fast paths need no check: Mesa never raises those vectors.
- A flag set by the glue was considered and rejected: a signal or fault taken between setting and clearing it misroutes, and the flag would have to nest.

Signals stay as now: a host signal while in Mesa becomes a guest interrupt through `amiga_sendsig`; its frame goes on the guest supervisor stack, never on the Mesa stack.

### 4.4 Where output goes

| Host | Warp3D renders into | How |
|---|---|---|
| Radeon session in front | the BitMap's place in the session's VRAM | `SetDrawRegion`: BitMap address − `MIG_RTG_VRAM` + the session's VRAM offset (backend) → colour offset and pitch of the r200 drawable; Z/stencil and back buffers in the same session's VRAM allocation |
| Any other host, or no card | the BitMap's memory | OSMesa into the session memory the container card maps; the same code on Mac, Atari and stock Amigas |

Rules from the display service:
- A hidden session gets no DRM lock: `LockHardware` and draw calls return `W3D_NOTVISIBLE` (a Warp3D error titles already handle), GL clients block in the lock.
- Switch-out: the kernel takes the DRM lock, waits for idle, saves the session's VRAM (front, depth) to its shadow, bumps the texture heap age so every context reloads its textures.
- The container card needs 16- and 32-bit modes first: it offers `MIG_RTG_INDEX8` only, and most Warp3D titles refuse CLUT screens.

### 4.5 Size

| Part | Lines |
|---|---|
| Warp3D.library hunk: romtag, 85 LVO stubs, stack/semaphore/FPCR glue | ~400 asm |
| ELF host side: state, textures, arrays, drawable, mode queries | ~3,500 C (Wazp3D's full replacement is 6.4k with its own rasteriser; its GL path is 1.6k) |
| startmig: table, Mesa link, `AMIGAIOC_HOSTPC` | ~100 |
| guestcore / amigaguest: PC range | ~40 |
| DRI host without X (screen, one drawable per session, cliprect = session) | ~800 + `dri_util.c` 1.1k reused |

## 5. Task order

| # | Task | Needs | Test where |
|---|---|---|---|
| 1 | gcc 2.95.3 + newer binutils for m68k SVR4 | – | host, then QEMU q800 (rebuild of a few modules) |
| 2 | Mesa 5.0.2 OSMesa static; `osdemo` | 1 | QEMU q800 |
| 3 | Software libGL (Xlib driver) with X11R6.3 client libs; `glxgears` on Xamix | 2 | QEMU q800 |
| 4 | Container card 16/32-bit modes; Warp3D.library + glue + host table + PC range; OSMesa backend; per-call counters | 2 | QEMU q800 (Amiga environment), Amiberry; Cow3D, WarpTest, StarshipW3D, then the user's titles to fix the tiers |
| 5 | DRM core port as DLM with a null driver: open, ioctl copy, SAREA map, lock, session auth, SG alloc | 1 | QEMU q800 (no PCI needed) |
| 6 | `opci_dmaalloc`, `opci_busaddr`, `opci_intr` exercised by a bus-master card | – | Amiberry (ES1370, a bus master, behind its emulated bridge) |
| 7 | Radeon display backend: POST (x86emu running the card ROM through `OPCI_LEGACYIO`/`opci_region`), PLL/CRTC/DAC from `radeon_driver.c` (~2k), VRAM per session, `SURFACE_CNTL` swap | 6 | **real card** |
| 8 | Radeon DRM: kernel CP init, PCI GART on the bridge's DMA memory, §1.4 swaps; check Prometheus bus mastering | 5, 7 | **real card** |
| 9 | r200 DRI driver with the X-less DRI host; a GL demo in a display session | 2, 8 | **real card** |
| 10 | Warp3D on r200 | 4, 9 | **real card** |
| 11 | Xamix: direct-only GLX + DRI extension; libGL with DRI loader | 3 (protocol part), 9 (rendering) | QEMU for the protocol, **real card** for DRI |
| 12 | Options: radeon (R100) driver, Radeon 2D acceleration in Xamix, CTPCI on the CT60 | 8 | real cards |

Card-free first: 1-6 and the protocol half of 11. Amiberry has no Radeon, and QEMU's q800 has no PCI.
