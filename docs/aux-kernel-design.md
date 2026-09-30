# A/UX personality for the AMIX-based SVR4 kernel: design

Design for running A/UX 3.1's Mac environment (`startmac`, `libmac1_s`, `Patch.067C`, System 7.0.1, CommandShell) unmodified on the SVR4.0 kernel built from the AMIX 2.1 relink kit plus the AMIX 040/060 port. The kernel is extended by linking new objects and overriding stock functions. The design is unimplemented.

Sources: the other `docs/*.md`, the AMIX kit (relocatable `exp` objects, Commodore source `amiga/ml/vec.s`, `ttrap.s`, `master.d/*.c`), AMIX headers, and A/UX 3.1 binaries and headers (`/unix`, `/usr/include/sys/uinter.h`, `sys/file.h`, `sys/user.h`). AMIX addresses are `.text` offsets inside the named object (`os` = `os/exp`, and so on). A/UX addresses are `/unix` addresses. *(verify)* marks something to check before implementing, and *(uncertain)* marks an inference.

`tools/elfxref.py <elf>... -- sym...` lists every relocation that references the given symbols, with the containing function; the call-site facts below come from it.

## 0. Summary of decisions

| Topic | Design |
|---|---|
| Personality marker | `p_evpdp` (proc+0xc8, the unused SVR4 events pointer) points to our `struct aux_proc`; non-NULL means the A/UX personality |
| Lifecycle hooks | The AMIX **events stubs** in `master.d/stubs.c` (source): `ev_config`, `ev_fork`, `ev_exec`, `ev_exit`. Fork, exec and exit need no binary patch |
| Exec | `execsw[0]` (magic 0x150) → `aux_coffexec`: A/UX images go to our loader (A/UX `.lib`, libraries at 0x47c00000…); SVR3 COFF with `STYP_LIB` goes to stock `coffexec` |
| Syscall entry | **Vector-level gate** for vectors $80 (trap #0) and $BC (trap #15), installed the way the 040/060 port installs its own handlers: by retargeting the `M68Kvec` slot's relocation from `nullvect` (§1a). The stub tests `curproc->p_evpdp`, then jumps to `aux_systrap` or to `nullvect`. Native cost is about 5 instructions per syscall (to be measured, §1a) |
| CPU virtualization | Same gate for $28 (A-line, with an assembly fast path), $20 (privilege), $24 (trace), the fault vectors (Mac reflection) and $F4 (060 unimplemented integer; chains to the port's `isp61_vec`). The virtual IPL holds signals through an `fsig` override |
| Address space | Override `valid_usr_range` (wrap-safe; quadrants 0 and 1 allowed for A/UX processes). Quadrant 1 (ROM, `/shlib`) is safe: the kernel window lives only in the SRP tree. **Quadrant 0 is not safe on the port as it stands**: its ITT0/DTT0 transparently translate 0–1 GB for user mode too, so Mac RAM at 0 and A/UX text at 0x10000000 require ITT0/DTT0 to be made supervisor-only (§1a). Keep AMIX's stack top 0xc0800000 |
| Signals | Wrap `sendsig` (A/UX processes → `aux_sendsig` with A/UX SVR3/BSD frames). `sigcleanup` and `sysm68k(2)` are handled in `aux_systrap` |
| uinter | Plain character driver (not STREAMS) with the `cdevsw` poll entry. `clock_hook` for `Ticks`/`Time`, `timeout()` for the SIGIOT tick. ROM services replaced by a fake NuBus video card, a kernel Slot Manager over a synthetic declaration ROM, file-backed PRAM, and synthetic kernel low-memory tables |
| Sockets | Both options specified (§7). The choice is open |
| AppleTalk | A/UX-facing shim (devices, ioctls, event ring, `atp_control`) over a provider interface. The stack choice stays open (§8) |

## 1. Hook inventory

Techniques:

- **S**: edit Commodore/`master.d` source (`kernel.c`, `stubs.c`, `vec.s`, `ttrap.s`, console drivers).
- **O**: override a global symbol (weaken the stock one and link ours).
- **W**: wrap a global symbol. Weaken it, add a global alias `__amix_X` at the stock address (`objcopy --add-symbol`), and our `X` calls `__amix_X`. Plain `--redefine-sym` is **wrong** for wrapping: it also renames references inside the defining object, which would then bypass the wrapper. For example, `psig` → `sendsig` are both in `os`.
- **A**: add a global alias to a static function so our code can call it.
- **R**: reimplement the function in C from the shipped binary.
- **T**: retarget one relocation in the linked image (a vector slot, a `cdevsw`/`execsw` field, a call operand) with a fail-closed script. This is the port's own technique (`src/patch_isp_vec61.py`, `patch_fpsp_vectors.py`, `patch_va2000_cdevsw.py`).

AMIX global/static status below was read from the ELF symbol tables of the 2.1 kit. **Build on the port (§1a):** the port patches the linked 2.1c `stand/unix` (an `ld -r` image) and never rebuilds from the kit's source. So every **S** row below becomes **T** (tables, vector slots) or **O**/**W** (the `ev_*` stubs are ordinary global functions in `stand/unix`). The objects and offsets are those of the kit; derive the 2.1c ones from `stand/unix`'s symbols at build time.

| AMIX symbol (object, offset, binding) | Tech. | Purpose |
|---|---|---|
| `execsw[]` (`master.d/kernel.c` 426) | S | entry 0 → `aux_coffexec` (dispatcher); optional AppleSingle entry |
| `cdevsw[]` (`kernel.c`; free majors 42, 43, 47–49, 51–69) | S | `uinter`, AppleTalk shim devices, `auxsock` (option a) |
| `init_tbl[]`/`io_init[]` (`kernel.c`) | S | `aux_init()` |
| `ev_config`, `ev_fork`, `ev_exec`, `ev_exit` (`stubs.c`, `EVENTSTUB`) | S | removed from the stub list and defined by us (§2) |
| `M68Kvec` slots (`amiga/ml/vec.s`) | S | gate stubs for $20 $24 $28 $80 $BC and the fault vectors (§3, §4) |
| `ttrap.s` | S | none needed with the gate design; optional variant in §3.3 |
| `valid_usr_range` (vm 0x773e, global) | W | quadrant 1 and wrap-safe end for A/UX processes (§4) |
| `sendsig` (os 0x1d1ea, global; sole caller `psig`+0xea) | W | A/UX frames (§5) |
| `fsig` (os 0xbd5e, global; sole caller `issig`+0xc0) | O | virtual-IPL signal holding (§4.5); 116 bytes, rewritten entirely |
| `usrxmemflt` (os 0x1f0a6, **static** in the kit) | A → W on the port | Mac bus-error reflection after page-fault resolution (§4.4). The port already globalizes and replaces it (`usrxmemflt_stock` alias at 2.1c .text 0x5aede), so we wrap the port's version rather than the stock body |
| `clock_hook` (common, called at `clock`+0xe with the `pcb_t *`) | runtime pointer (chained) | `Ticks`/`Time` update (§6.5) |
| `u_trap` (os 0x1e646, global), `systrap` (os 0x1eaf6, global) | untouched | `aux_systrap` copies `systrap`'s tail logic; A/UX processes never enter `systrap` |
| Amiga console keyboard/mouse (`amiga/console/*`, source) | S | input diversion to uinter (§6.6) |

Existing global kernel services the personality calls (all global unless noted): `psignal`, `sigtoproc`, `setrun`, `sleep`, `wakeup`, `timeout`, `untimeout`, `prfind`, `kmem_alloc`/`kmem_zalloc`, `lfuword`, `copyin`/`copyout`, `lookupname`, `execmap`, `remove_proc`, `setexecenv`, `exhd_getmap`, `as_map`, `as_unmap`, `as_fault`, `as_ctl`, `segvn_create`, `segdev_create`, `hat_getkpfnum`, `vtop`, `useracc`, `strioctl`, `strpoll`, `pollwakeup`, `falloc`, `getf`, `setf`, `closef`, `fpu_save`/`fpu_restore` (ml), `preempt`, `issig`, `psig`, `addupc`. Statics we need: `trapsig`, `usrxmemflt`, `getcoffhead` (reimplemented rather than aliased), `shmat` (not needed: it calls `valid_usr_range` through a relocation, so the wrapper applies).

## 1a. Fit with the AMIX 040/060 port

Based on the port's `relink-040.sh`, `src/*.s`, `src/patch_*.py` and `docs/contracts/DTT0-NARROWING-SPEC.md`.

### How the port handles vectors and trap entry

- **Build model.** `relink-040.sh` starts from the installed `stand/unix` (2.1c, a relocatable `ld -r` image). It applies `objcopy` weaken/`--add-symbol X_orig` (our **W**), links override objects with `ld -r`, then runs fail-closed relocation and byte patchers. `vec.s`/`ttrap.s` are never reassembled.
- **Vector table.** `M68Kvec` stays in `.text` and VBR still points at it. Each slot is a long with an `R_68K_32` relocation to its handler. The port retargets slots by rewriting that relocation:
  - `patch_fpsp_vec11.py`: vector 11 (F-line) → FPSP glue;
  - `patch_fpsp_vectors.py`: 48, 51–55, and 60 on 060 → FPSP;
  - `patch_isp_vec61.py`: 61 → `isp61_vec`, a bounded 060 unit for the one 64-bit multiply form measured in AMIX userland. Anything else falls back to `jmp nullvect`.

  Slots 8 ($20), 9 ($24), 10 ($28), 32 ($80) and 47 ($BC) are still `nullvect`. The patchers assert "current target is `nullvect`" before retargeting.
- **`nullvect` is weakened** (`nullvect_orig` at 2.1c .text 0x11b4). `kvecprobe040.s` supplies a counting wrapper, off by default.
- **`utraps` → `u_trap` call.** The operand (reloc at .text 0x11f0) is retargeted in the base build to `srg_utraps`, a latch that tail-jumps to `u_trap`. Debug builds chain `kvd_utraps` before it. `ureturn` is untouched, and `isp61_vec` itself exits through `jmp ureturn`.
- **Other overrides.** `resume`, `setregs`, `get_fault`, `userspace` (format-7/format-4 aware, classifies by function code), `usrxmemflt`/`krnxmemflt`, `hardbus`, `vtop`, `brk`, `mprotect`, `copyout`, `as_fault`, FPU save/restore, and the HAT. `sendsig` is byte-patched in place (`patch_sendsig_fpu.py`, an `fpu_present` gate on its `fpu_setup` call).
- **None of our hook symbols collide** except `usrxmemflt` (wrap the port's version) and `sendsig`. Our W on `sendsig` keeps the stock body, including the port's in-place patch, behind the alias; the order is port patch, then our weaken/alias, which is fine because the alias points at the patched bytes.

### Where the A/UX gates fit

- The gates are new override objects, and a new patcher `patch_aux_vectors.py` in the port's style retargets slots 8, 9, 10, 32 and 47 (plus 3, 4, 5, 6, 7 for Mac fault reflection) from `nullvect` to `aux_gate_*`. It runs after the port's FPSP/ISP patchers and asserts their results.
- Slot 61 is retargeted from `isp61_vec` to `aux_gate_61`, which chains to `isp61_vec` for everything that is not a Mac task.
- The gates' native fall-through is `jmp nullvect`, which resolves to whatever the port currently binds (`kvecprobe` wrapper or `nullvect_orig`), with the raw frame untouched, exactly as `isp61_vec` does.
- The gates' A/UX path exits through `jmp ureturn`, like `isp61_vec`.
- Entry register save: copy `nullvect_orig`'s sequence (`moveml %d0-%fp,-(%sp)`, `movel sup_cacr,%d0`, `movec %d0,%cacr`). The port keeps this sequence.
- `execsw[0].exec_func`, the new `cdevsw` rows and an `io_init` or `init_tbl` slot for `aux_init` are relocation retargets inside `kernel.o`'s data in `stand/unix`. `ev_config`/`ev_fork`/`ev_exec`/`ev_exit`/`ev_istrap` are weakened and overridden. So **the per-process-vector approach survives unchanged**; only the installation technique changes from source edit to relocation retarget.
- **Gate cost must be measured.** The port measured its own `nullvect` wrapper at 7.74 µs per syscall (7.4% of system time) on an A3640 (`kvecprobe040.s` header). Kernel `.data`/`.bss` sit in the DTT0 identity window, which is cache-inhibited, and that wrapper also pushes and counts. Our gate reads only `curproc` (NC) and `p_evpdp` (proc structures are in PTE-mapped, cacheable kernel VA).
- If the gate cost is visible, use the VBR variant (§3.3). The port **owns `resume`** as source (`runtime040.s`), so a `movec` of `AUXvec`/`M68Kvec` into VBR there covers every context switch, including a child's first run. Also set VBR at `ev_exec` when the personality of the running process changes. Build `AUXvec` at `aux_init` by copying `M68Kvec` after all link-time retargets.

### Transparent translation: supervisor-only?

| CPU | Register | Value (port) | Match | Function-code field | Consequence |
|---|---|---|---|---|---|
| 040/060 | ITT0 | 0x003fc000 (`pstart040.s`) | 0x00000000–0x3FFFFFFF, WT | S = 10: **user and supervisor** | user instruction fetches below 1 GB bypass the page tables |
| 040/060 | DTT0 | 0x003fc060 | 0x00000000–0x3FFFFFFF, cache-inhibited | S = 10: **user and supervisor** (the spec's own table says "user and supervisor") | user data accesses below 1 GB go to physical memory |
| 040/060 | DTT1 | 0x807fa060 | 0x80000000–0xFFFFFFFF, cache-inhibited | S = 01: supervisor only | no user effect |
| 040/060 | ITT1 | 0 | disabled | — | — |
| 030 | TT0/TT1 | never written by the kernel (`tt0_on` 0x003F0143 / `tt1_on` 0x807F0143 in `ttrap.s` have no references; both are FC 4–7 = supervisor and E = 0) | — | — | *(verify the loader leaves TT0/TT1 disabled)* |

- **Quadrant 1 (0x40000000–0x7FFFFFFF): no collision.** No TT register covers it. The kernel's quadrant-1 window is only in the SRP tree: `kroot040` root[32..63] via `kptr040`, and `hat040.s` states "SRP=kroot040, so NO kernel entries are copied" into the 128-entry per-process user roots. User mappings there go through the process's own URP tree, so the ROM at 0x40800000 and `/shlib` at 0x47c00000–0x47fc8000 are safe on 030, 040 and 060.
- **Quadrant 0 (0–1 GB): collision on 040/060.** AMIX user space is normally ≥ 0x80000000 (ELF text at 0x80800000, stack 0xc0800000), so the port has never run user code below 1 GB. An A/UX Mac process lives almost entirely there:
  - Mac RAM at 0 (shm `tLOW`), with the ui page 0x3000 and `Patch.067C` at 0x4000;
  - `startmac`/CommandShell text at 0x100001e8;
  - A/UX `/bin/sh` at 0xa8 and data at 0x401d10.

  With ITT0/DTT0 matching user mode, these accesses would be identity-mapped to **physical** memory (kernel image, chip RAM, custom chips) instead of the process's pages. The result is silent corruption and no fault.
- **Required change to the port: make ITT0/DTT0 supervisor-only**, S = 01: ITT0 0x003fa000, DTT0 0x003fa060. The same immediate sites in `pstart040.s` (and `haltsys040.s`, which clears them) are affected. The port's spec already lists this as the "cleaner final policy" (N1: `0x0000a060`), to be taken "after proving that no supported user ABI relies on transparent low addresses". Stock AMIX on the 030 never gave user mode transparent low access (TT disabled; quadrant 0 only in the SRP tree's early-termination entry), so no native ABI can rely on it. The change affects only user-mode accesses; all kernel uses of the identity window are supervisor.
- **Other port code that assumes "user VA ≥ 0x80000000"** must learn about A/UX processes:
  - `vtop040.s`: with a proc, `va < 0x40000000` → identity, and 0x40000000–0x7FFFFFFF → "contract violation", forcing a kernel walk. A DMA to an A/UX user buffer (raw I/O via `physio`, `alien/dd.c` `vtop(b_addr, b_proc)`) would get a wrong physical address. The fix: when `b_proc` has `p_evpdp` and the VA is below 0x80000000, use the user walk. Keep the identity case only for the known bounce-buffer shape (`dma_pageio`, which the port documents).
  - `getfault040.s`'s debug filter ("0x40000000..0x7FFFFFFF = kernel") only affects logging.
  - *(verify)* `hat040.s`/`uvatosde040.s` build user root entries 0–63 as generically as 64–127: never exercised, since stock `valid_usr_range` forbade quadrant 1 and AMIX never mapped quadrant 0 for user.
- Coherency note for §6.5: the port runs CACR 0x80008000 (040/060 instruction and data caches on, data **write-through**; `hat_cm_ram` = WT). The spec states that an NC access through DTT0 dislodges a matching physically tagged line, so kernel NC writes into user pages are coherent without `cpushl` while user RAM is WT. If the port ever enables copyback for user pages, `aux_upoke` needs the `cpushl` of §6.5. The port's `wb040.s` depends on SFC/DFC across interrupted `moves` loops, so gates and `aux_upoke` must save and restore them.

## 2. Process identity

### 2.1 Marker and per-process state

- **Marker**: `p->p_evpdp` (proc+0xc8; offset checked against `sys/proc.h` and `u_trap`'s `tst.l $c8(a3)`). AMIX links the events stubs (`EVENTSTUB` in `stubs.c`), so the field is never used. Kernel references found in os/fs/vm/io/disp/exec: `u_trap`+0x45c, `systrap`+0x308, `sigtoproc`+0xce and `sleep`+0xd0/+0x19a. Each only calls `ev_istrap(p)` when the field is non-zero. Our `ev_istrap` returns 0, so behaviour stays stock, at the cost of one extra call per trap return for A/UX processes. *(verify: scan netinet, rpc, klm, ktli, des and amiga drivers for `$c8(` on proc pointers.)*
- Fallback if the field turns out to be unusable: a table indexed by `p->p_pidp->pid_prslot`, plus p_flag bit 0x80000000 as the fast marker. No AMIX code in os/disp/vm tests p_flag bits 28–31; the only `btst #4..#7,$4(aN)` hits are in `semop`/`semundo` on a different structure.
- `struct aux_proc` (kmem, not swapped):

```c
struct aux_proc {
    struct proc      *ap_proc;
    u_int             ap_flags;     /* APF_LOADING, APF_UISET, APF_LOWMEM ... */
    u_int             ap_compat;    /* A/UX compat word, default 0x403 */
    u_int             ap_sv_onstack, ap_sv_intr;  /* per A/UX signal bit */
    caddr_t           ap_ss_sp;     /* sigstack top */
    int               ap_ss_onstack;
    int               ap_code;      /* BSD u_code for the next frame */
    u_short           ap_vsr;       /* virtual SR (u+0x548 on A/UX) */
    u_char            ap_maclevel;  /* proc+0x87 on A/UX */
    struct aux_layer *ap_layer;     /* NULL = not a Mac task */
    short             ap_task;      /* l_attached[] slot, -1 */
    u_long            ap_lm_pa[4];  /* phys pages of Mac VA 0..0x3fff once UI_SET */
    struct aux_itimer ap_itreal;    /* ITIMER_REAL callout */
    char             *ap_cwd;       /* cwd path for fidop/csop */
    u_int             ap_gfd_gen;   /* global-fd mirror generation (§6.3) */
};
```

`hostid`, `tz`, `_fmgrflag`, the kmsgq queues and the layer table are global.

### 2.2 Exec

- `gexec` (os 0x1b888) calls the **first** `execsw` entry whose magic matches and stops there (`bra $1ba28` after the call). So the 0x150 entry must be a dispatcher. `execsw[0] = {&coffmagic, aux_coffexec, coffcore}`.
- `aux_coffexec(vp, uarg, level, npages, exhda)` reads the file and section headers (reimplementing static `getcoffhead` with A/UX rules; see [amix-coffexec-vs-aux.md](amix-coffexec-vs-aux.md) §4a) and classifies:
  - `STYP_LIB` 0x800 section present → an SVR3 image → tail-call stock `coffexec`.
  - Otherwise → A/UX. `.lib` is STYP_INFO 0x200 (64-byte paths, count = size/64), `.lowmem`/`.low24` by name. A static A/UX binary such as `/bin/sh` looks exactly like any SVR3 COFF (flags 0x203, vstamp 0), so **all non-`STYP_LIB` 0x150 files are A/UX**. A tunable `aux_coff_default` (1 = A/UX) keeps a way back. `.low24` images → `ENOEXEC` (24-bit sessions unsupported).
- A/UX load sequence, mirroring stock `coffexec`:
  1. Validate the libraries (lookupname, VREG, `VOP_ACCESS(VEXEC)`, 0x150 + text/data/bss, a.out magic 0x108/0x10b, F_EXEC not required).
  2. Set `APF_LOADING` in a per-exec flag (`aux_loading = curproc`).
  3. `remove_proc(uarg)` (os 0x1c040, the point of no return). Inside it `ev_exec(p)` runs (remove_proc+0x3e), and this is where the personality is set:

     ```c
     void ev_exec(proc_t *p)
     {
         struct aux_proc *ap = AUXP(p);
         if (aux_loading == p) {            /* A/UX image */
             if (!ap) ap = aux_proc_new(p); /* native -> A/UX */
             aux_proc_exec_reset(ap);       /* compat 0x403 unless COMPAT_EXEC,
                                               sigstack, vsr, detach from layer,
                                               itimer */
         } else if (ap) {
             aux_proc_free(p);              /* A/UX -> native; p_evpdp = 0 */
         }
     }
     ```

  4. `execmap` each library (text 0xd, data 0xf + bss), then the program; `setexecenv`. The libraries' text is not congruent with its file offset, so `execmap` copies it (as A/UX's `loadreg` does). Sharing can come later.
- Native exec from an A/UX process (ELF, SVR3 COFF, `#!`): the stock loader's `remove_proc` → `ev_exec` frees the state. `#!` scripts take the interpreter's type.
- D0 at entry: AMIX `setregs` sets D0 = sp, A/UX sets 0. A/UX `crt0` only stores it in `splimit%`, which is harmless. When the caller is an A/UX process, `aux_systrap`'s `exece` path clears D0 after a successful exec. Native parents: accept.
- Later: an AppleSingle/AppleDouble `execsw` entry (first short 0x0005) that re-execs `/mac/bin/launch file`, as A/UX `gethead` does *(argv convention: check `gethead` 0x1001621a)*.

### 2.3 Fork and exit

- `procdup` (os 0x5a08, static) runs `if (ev_config() && ev_fork(pp, cp)) fail;` (+0x44/+0x54), in the parent, before the child can run. On a later failure it calls `ev_exit(cp, 0)` (+0x78).
  - `ev_config()` returns `curproc->p_evpdp != 0`. It is also consulted by `hrtalarm`, `hrtsleep` and `hrtcancel` (io; ENOPKG otherwise). Those are `hrtsys` paths, which A/UX processes never reach: the personality implements itimers itself (§2.2 of the translation spec).
  - `ev_fork(pp, cp)` allocates the child's `aux_proc` and copies compat, signal flags, sigstack, vsr, cwd and tz. The child is **not** a layer task: A/UX's child of a Mac process keeps the Mac vectors until its exec *(uncertain; `fork_exec` children exec immediately)*.
- `exit` (os 0x34a4) calls `closeall` first, then `hdeexit()` (+0x116) and `ev_exit(p, stat)` (+0x12c), and only then `relvm`. `ev_exit` does, for an A/UX process:
  - layer detach (post `exitEvt`, free the attach slot, `ui_terminate` if it was the last task);
  - cancel the itimer callout;
  - drop kmsgq entries;
  - free `aux_proc` and set `p_evpdp = 0`.

  The address space still exists at this point, so unlocking Mac low memory is still possible.

## 3. System-call entry

### 3.1 Where stock AMIX goes

`vec.s` sends every non-interrupt vector to `nullvect`, then `utraps`, then `u_trap(usp)`. `u_trap` dispatches on vector/4 through a jump table at os 0x1e6a6. Decoded:

| Vector | AMIX action |
|---|---|
| 32 trap #0, 42 trap #10 | `systrap` |
| 33 trap #1 | PC −= 2, SIGTRAP (breakpoint) |
| 10 A-line, 11 F-line, 34–41 and 43–47 (trap #2–#9, #11–#15) | SIGSYS (12) |
| 8 privilege | SIGILL |
| 9 trace | SIGTRAP |
| 2 bus error | `usrxmemflt`, then SIGSEGV/SIGBUS |

So A/UX `trap #15` would give SIGSYS on a stock kernel.

`systrap` facts that `aux_systrap` must reproduce:

- `u_ar0` = u+0x864; frame: +0 usp, +4 d0 … +0x40 SR, +0x42 PC, +0x46 format/vector.
- args via `lfuword` into `u_arg` (u+0x784), `u_ap` (u+0x734).
- `u_rval1/2` = u+0x738/0x73c, preloaded r_val2 = caller's d1.
- `u_error` = u+0x726; `setjmp(u_qsav)` at u+0x6f0 when the sysent flag bit 0 is set.
- carry via `ori.b #1,$41(ar0)`.
- `EFBIG` → `psignal(SIGXFSZ)`.
- `EINTR` → `ERESTART` (91) if `p_cursig`'s bit is in u+0x5e4 (SA_RESTART mask).
- tail: `preempt` if `runrun`; `issig`/`psig` if `p_cursig` (+0x86) or `p_sig` (+0x9c) or `SPRSTOP` (p_flag 0x100); `addupc` if profiling (u+0x874).

### 3.2 Chosen routing: gate stubs in the vector table

Slots $80 and $BC point at small gates. On the kit this is a `vec.s` edit; on the port it is a relocation retarget of the slot (§1a). They cost a native syscall about 5 instructions (under 1% of `systrap`), and A/UX calls never pass through `u_trap`/`systrap`:

```
aux_gate_sys:                       | vectors $80, $BC
        btst    #5,(%sp)            | from supervisor? (never for traps)
        bne     nullvect
        move.l  %a0,-(%sp)
        movea.l curproc,%a0
        movea.l 0xc8(%a0),%a0       | p_evpdp; movea leaves CCR alone
        cmpa.w  #0,%a0
        movea.l (%sp)+,%a0
        beq     nullvect            | native: stock path, unchanged
        movm.l  &0xfffe,-(%sp)      | same frame as nullvect/utraps
        mov.l   sup_cacr,%d0        | (040/060 port: its own entry sequence)
        mov.l   %d0,%cacr
        mov.l   %usp,%a0
        mov.l   %a0,-(%sp)
        jsr     aux_systrap         | C: aux_systrap(pcb_t *)
        mov.l   (%sp)+,%a0
        mov.l   %a0,%usp
        bra     ureturn             | global in ttrap.s: STREAMS, cl_trapret,
                                    | s_trap, sigflag stack fix-ups
```

The fault and CPU vectors in §4 use the same gate shape with other C targets.

### 3.3 Rejected and alternative routings

- **`systrap`/`u_trap` hook** (wrap `u_trap`, test the personality in C): every A/UX syscall first runs the `u_trap` prologue (bzero of siginfo, the jump table). Trap #15 would need a new jump-table target, and `u_trap` is binary. The gate is simpler and cheaper.
- **Second vector table plus VBR switch.** On the port the natural place is its own `resume` (`runtime040.s`, source), plus `ev_exec` (§1a). The kit alternative follows: add `movec` of `AUXvec`/`M68Kvec` at `stkcheck:` in `ttrap.s`. Every return to user mode passes `trap_ret3` → `stkcheck`, including first returns of forked children, because they return through `systrap` → `ureturn`. `p5int` returns with a bare `rte` but never changes process. This gives zero native cost, but `AUXvec` must be rebuilt whenever the port or a debugger changes `M68Kvec` (for example FPSP/060SP installs). Keep it as an optimisation if profiling ever shows the gate.

### 3.4 `aux_systrap(pcb_t *)`

```c
void aux_systrap(pcb_t *f)
{
    u.u_ar0 = f; ...                     /* as systrap: sysinfo, u_error=0 */
    int trap15 = (FVEC(f) == 47);
    int num = f->d0 & 0xff;
    if (trap15 && num == 150) { aux_sigcleanup(f); goto out; }
    if (!trap15 && num == 0) {           /* indirect */
        num = lfuword(f->usp + 4); argbase = f->usp + 8;
    }
    ent = &auxsysent[num];               /* A/UX numbering, 0..169 */
    if (trap15)
        fetch a0,d1,a1,d2,a2,d3 from f;  /* wait3: CCR==0x1f on entry */
    else
        lfuword x argc from usp+4;
    rv[0] = 0; rv[1] = f->d1;
    err = (ent->flags & AUX_QSAV) && setjmp(&u.u_qsav)
              ? (u.u_error ? u.u_error : EINTR)
              : ent->fn(args, rv);        /* class N/C/K/U/S handler */
    if (err == EINTR || err == ERESTART) {
        if (aux_restartable(ap, num)) {  /* §1 rule of the translation spec */
            f->pc -= 2; goto out;        /* d0 and args untouched */
        }
        err = EINTR;
    }
    if (err) {
        if (err == EFBIG) psignal(p, SIGXFSZ /* AMIX 31 */);
        f->d0 = aux_errno_out(err, ap, fd_kind);  /* SVR4 -> A/UX, EWOULDBLOCK */
        f->sr |= 1;
    } else {
        f->d0 = rv[0]; f->d1 = rv[1]; f->sr &= ~1;
    }
out:
    aux_mac_ret_hook(ap);                /* Ticks catch-up, global-fd sync (§6) */
    tail as systrap: preempt(), issig()/psig(), addupc()
}
```

- The handler table uses the classes and rows of [aux-syscall-translation.md](aux-syscall-translation.md) §2 and §6. N-class entries call the AMIX `sysent` handler (`sysent` os data 0x6f8, 8-byte entries, handler at +4) with our `uap` and an `rval_t`.
- A/UX `nosys` numbers → `psignal(SIGSYS)` and no error, as A/UX does.
- Truss/`/proc` syscall stops (u+0x418 masks) are not supported for A/UX processes at first.

## 4. Address space and CPU virtualization support

### 4.1 User and supervisor spaces are separate

- 030/68851: `tc_on` = 0x82B02D60 has **SRE** (bit 25) set, and `pstart` loads the kernel tables into **SRP** (ml +0xfd6). User processes use CRP; `hat_alloc` gives each process its own 4-entry root. The kernel reaches user memory only through `moves` with SFC/DFC = 1 (`lfubyte` ml 0x3fa and the others, recovery pointer `sf_fault` at u+0x374).
- `userspace()` (os 0x1f7b8) classifies bus-error faults by the frame's **function code**, not by address.
- 040/060: URP and SRP are always selected by privilege.
- So user mappings at 0x40000000–0x49FFFFFF (ROM 0x40800000, libraries 0x47c00000–0x47fc8000) cannot collide with the kernel window (`u` 0x40000000, `syssegs`, `kvsegmap`, `kvsegu`).
- On the port (§1a), ITT0/DTT0 (0–1 GB) match **user and supervisor** accesses. User accesses to Mac RAM at 0 and to A/UX text at 0x10000000 would bypass the MMU. **ITT0/DTT0 must become supervisor-only** (0x003fa000 / 0x003fa060). DTT1 is already supervisor-only, and no TT register covers quadrant 1. On the 030 the kernel never loads TT0/TT1.

### 4.2 `valid_usr_range` wrapper

Stock rule (vm 0x773e): reject if `addr+len <= addr` (wrap), if `addr` is in quadrant 1, or if `addr` and `addr+len` are in different quadrants. Note that `end` is exclusive: a range ending exactly at a 1 GB boundary is rejected, and a range ending at 2³² wraps to 0 and is rejected.

```c
int valid_usr_range(caddr_t a, size_t len)
{
    if (!AUXP(curproc)) return __amix_valid_usr_range(a, len);
    u_long e;
    if (len == 0 || (e = (u_long)a + len - 1) < (u_long)a) return 0;
    return ((u_long)a >> 30) == (e >> 30);   /* no quadrant crossing */
}
```

- The quadrant-crossing restriction stays because the HAT allocates per-quadrant tables *(uncertain whether crossing is actually unsafe)*.
- Callers are `shmat`, `mmap`, `munmap`, `mprotect`, `mincore`, `memcntl` and `seg_alloc`, so exec mappings, shm and device mappings all get the rule. During exec `curproc` is the exec'ing process, and `ev_exec` has already set the personality before `execmap`.

### 4.3 Layout of an A/UX Mac process on AMIX

| Range | Mechanism | Notes |
|---|---|---|
| 0 … memsize (Mac RAM, shm `'tLOW'`) | A/UX `shmat(id, 1, SHM_RND)` | Works unmodified: AMIX `shmat` (os 0x19480, static) treats only addr 0 as "choose"; with SHM_RND it rounds 1 to 0 (`&~0x7ff`, SHMLBA; 4 KB on the port) and `valid_usr_range(0, …)` is quadrant 0. **On 040/060 this needs ITT0/DTT0 made supervisor-only (§1a)**; the same applies to all quadrant-0 rows (ui page, `Patch.067C`, program text/data) |
| 0x3000 ui page | inside tLOW; `UI_MAP` locks it (§6) | |
| 0x10000000 program, 0x47c00000–0x47fc8000 `/shlib` | `aux_coffexec` | quadrant 1 via §4.2 |
| 0x40800000 ROM | `UI_ROM` → `as_map(segdev)` over the ROM object (§6.7); or TBRAM shm copy | quadrant 1 |
| 0xFs000000 NuBus slot space (fake card) | `UI_PHYS_SCREENS` → `segdev` | quadrant 3, above AMIX `UVEND` 0xf1000000; *(verify the HAT and `as_gap` accept VA above UVEND)* |
| 0xFFFF8000–0xFFFFFFFF 060 helper page | `UI_SET` on 060 hosts (§4.7) | needs the wrap-safe end of §4.2 |
| stack 0xc0020000–0xc0800000 | AMIX default | **Keep AMIX's stack top.** `stackfault` (os 0x1f5ec) hard-codes the growth window 0xc001ffff–0xc07fffff, and `grow`, `p0init` and `extractarg` hard-code 0xc0800000. A/UX's 0x40000000 top for `.lowmem` images (A/UX `gethead`) would break stack growth. Nothing in the Mac environment is known to depend on it; the Mac code runs 32-bit only |

`brk` is limited below 0xc0000000 (`brk` os 0x1c2cc), which is fine for A/UX heaps above 0x10047000. `map_addr`'s default search is 0xc1000000 + 0x30000000, clear of all Mac ranges.

### 4.4 Per-process vectors: gates instead of A/UX's table rewrite

A/UX reloads the table's $20/$28 from the u-area in `resume` (`fp20`/`fp30`/`fp40`). AMIX's `resume` is binary (ml 0x9c, called only from `swtch`+0x232), and the table is shared. So each relevant slot gets a gate (§3.2 shape), installed by relocation retarget on the port (§1a). The port also owns `resume` as source, so A/UX's per-process table rewrite could be copied literally there. The gates are kept because they need no per-switch work. The gate tests the personality and then the per-process Mac state in `aux_proc`:

| Vector | A/UX process, `APF_UISET` (Mac task after `UI_SET`) | A/UX process without it | native |
|---|---|---|---|
| $28 A-line | **reflect** to user `$28` (fast path, below) | `nullvect` (AMIX SIGSYS → A/UX 12). A/UX's `lineAFault` goes to `trap()` *(A/UX signal not traced; SIGILL likely)* | `nullvect` |
| $20 privilege | `aux_priv` emulator (§4.6) | `nullvect` (SIGILL) | `nullvect` |
| $24 trace | reflect to user `$24` with a format-2 frame, unless traced (`STRC`) or user `$24` is 0/odd (A/UX `utrace` 0x100124da) | `nullvect` | `nullvect` |
| $0C, $10, $14, $18, $1C, $C0–$D8 (address error, illegal, zero divide, CHK, TRAPV, FP) | **reflect** to the Mac vector at user address = vector offset, if that long is non-zero and even (A/UX `userhandler` 0x54d40). Copy the hardware frame to the user stack with vSR merged into SR; set `sigflag` `USTKCLEAR` (0x80000000, `sys/sysm68k.h`) so `ttrap.s` returns with a 4-word frame | `nullvect` | `nullvect` |
| $08 bus error | first `usrxmemflt` (alias to static os 0x1f0a6). If unresolved and the Mac `$08` is set → reflect as above; else stock `u_trap` | `nullvect` | `nullvect` |
| $2C F-line, $F0/$F4 (060 unimplemented EA/integer), $DC (060 unimplemented data type) | §4.7 | chain to the port's handler | chain |
| $80/$BC | `aux_systrap` | `aux_systrap` | `nullvect` |

Notes:
- `$80`/`$BC` are the only vectors `Patch.067C`'s `InitExceptions` leaves to the kernel (besides `$88`, trap #2). None of `Patch.067C`, `libc1_s`, `libmac1_s`, `startmac` or `/bin/sh` contains a `trap #2`, so $88 stays on `nullvect` *(the A/UX kernel's use of trap #2 is unknown)*.
- "Chain" means the gate jumps to the handler the port bound to that slot at link time: FPSP glue for 11, 48, 51–55 and 60 (060); `isp61_vec` for 61; `nullvect` otherwise. The gate references that symbol directly, or copies it into `aux_chain[vec]` at `aux_init`, so the port's FPSP/ISP keep working.
- Reflected long frames: the host CPU's native bus-error formats reach Mac handlers. That is what the ROM expects on the same CPU. On a 060 host running the 040 Quadra ROM, the formats differ (060 format 4, 040 format 7). This only matters for Mac code that inspects bus-error frames (hardware probing): see §10.

### 4.5 A-line fast path

The most frequent exception: every Toolbox call. Equivalent of A/UX `lineAVector` (0x1001244e), in assembly, without the full `movem`:

```
aux_linea:                        | $28 gate, Mac task with APF_UISET
        movem.l %d0/%a0,-(%sp)
        mov.l   &aux_la_slow,sf_fault   | u+0x374 recovery (as lfubyte)
        moveq   &1,%d0 ; movec %d0,%dfc ; movec %d0,%sfc
        mov.l   %usp,%a0
        mov.l   12(%sp),%d0             | PC.lo | format/vector
        moves.l %d0,-(%a0)
        mov.w   (ap_vsr),%d0 ; swap %d0 ; mov.w 10(%sp),%d0 ; or.l ...
                                        | SR|vSR : PC.hi
        moves.l %d0,-(%a0)
        moves.l 0x28,%d0                | user $28 = ROM dispatcher (Foreign OS entry 1)
        clr.l   sf_fault
        mov.l   %a0,%usp
        mov.l   %d0,10(%sp)             | new PC
        andi.w  &0x00ff,8(%sp)          | user mode, CCR only
        andi.w  &0x0700,(ap_vsr)        | vSR keeps IPL only
        tst.l   runrun ; bne aux_la_resched   | full frame -> ureturn
        movem.l (%sp)+,%d0/%a0
        rte
```

- The user frame is the 8-byte format-0 frame that `ATrap68020` consumes with `rts`/`rtd` ([aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md) §1).
- A fault on the user stack push goes to `aux_la_slow`, a C path that uses `suword` and posts SIGSEGV on failure.
- Unlike AMIX's `return`, the fast path skips the `cacr` switch. On the 030 this leaves the user caches as they were, which is correct because nothing else changed memory. On the port, CACR (0x80008000) is the same in both modes, so skipping the switch is harmless there too. The fast path must restore SFC/DFC, because interrupted `moves` loops (`wb040.s`) depend on them.
- The trace gate ($24) uses the same code with A/UX `utrace`'s 12-byte format-2 frame (instruction address, fmt 0x2024, PC, SR|vSR).

### 4.6 Privileged-instruction emulation (`lpriv` equivalent)

- C first: the gate saves the full frame and calls `aux_priv(pcb_t *)`. It decodes the opcode at PC with `fuword` and emulates the list A/UX's `lpriv` (0x10012710–0x10012f2c) handles:
  - `ori`/`andi`/`eori #,SR`; `move <ea>,SR`; `move SR,<ea>`; `movec` (CACR, USP, CAAR, SFC/DFC, VBR reads);
  - `rte` for formats 0, 2, 3, 9;
  - `move USP`; `moves`; `fsave`/`frestore`;
  - 040 `cinv`/`cpush`, which the kernel performs for the process's pages (`cpusha` is acceptable);
  - MMU stubs.

  Anything else → SIGILL.
- SR writes: the CCR and T bits go to the real frame. S, M and IPL go to `ap_vsr`. The IPL is mirrored into `layer->l_vipl`.
- On a lowering to 0 with `layer->l_pending` set: walk the layer tasks; for those with `p_sig & ~p_hold` non-zero, `setrun` if sleeping interruptibly (as `sigtoproc` does), else set `runrun`. The current task gets its signals on the way out (the gate exits through `ureturn` after the `issig`/`psig` tail). This is the `UI_sigpending` equivalent.
- An assembly fast path for the hottest forms (`move sr,-(sp)`, `move (sp)+,sr`, `ori/andi #,sr`) after profiling.
- Mac `movec` to CACR must actually flush on 020/030, because A/UX `CacheFlush` (sysm68k 0x69) returns EINVAL there and Mac code then uses CACR.

### 4.7 Virtual IPL and signal holding

- **Hook: `fsig` override** (os 0xbd5e, 116 bytes, sole caller `issig`).

  Stock logic: `m = p_sig & ~p_hold`, also `& ~holdvfork` if SVFORK; SIGKILL first; else the lowest set bit.

  Ours adds: if `AUXP(p)->ap_layer && layer->l_vipl != 0`, then `m &= SIGMASKBIT(SIGQUIT)|SIGMASKBIT(SIGKILL)|SIGMASKBIT(SIGTERM)` and `l_pending = 1` if anything was removed. This is A/UX `P_SIGMASK` 0x10027a54, where 3, 9 and 15 have the same numbers on both systems.
- **Why not `p_hold` manipulation.** `psig` saves `p_hold` into `u_sigoldmask` (u+0x5e8) for the frame, so inflated holds would leak into handler masks. `sigblock`/`sigsetmask` would have to edit a shadow mask. And the virtual IPL is **per layer** (A/UX global `ui_SR`), so every task's `p_hold` would need updating.
- Side effect: `sigtoproc` checks `p_hold` only, so a signal held by the virtual IPL still wakes an interruptible sleeper. `sleep` then finds nothing deliverable in `issig` and returns normally, and callers re-check their condition. SVR4 sleepers loop *(verify `sleep`'s PCATCH path at os 0xc7e8)*.
- The tick must not be lost: SIGIOT stays pending (one bit) while held, as on A/UX.

### 4.8 68060: unimplemented instructions in Mac processes

From [rom-060.md](rom-060.md):

- **Gate $F4 (vector 61)** for Mac tasks. Decode the instruction at PC. If it is a patchable 64-bit `muls.l`/`mulu.l` form and the page is writable RAM (tLOW, the private ROM object of §6.7, TBRAM copy), rewrite it with a same-length `jsr` (`jsr abs.w` → helper at 0xFFFF8xxx for 4-byte forms, `jsr abs.l` for 6-byte forms). Push the line (`cpushl`), and `rte` to the same PC. No emulation is needed, because the rewritten instruction runs. Otherwise chain to the port's `isp61_vec`. Note that it emulates only the one form measured in AMIX userland and otherwise ends in `nullvect` (SIGSYS). So the gate must also **emulate** the ROM's forms (`muls.l (d16,SP),D0-D1`, `muls.l D5,D6-D0`, `mulu.l D0,D0-D1`) wherever patching is refused (read-only or shared text).
- **Helper page**: a kernel-supplied read-only page mapped at 0xFFFF8000 into each Mac task at `UI_SET` (`segdev` over a kernel page). The helpers compute 64-bit products from 32-bit multiplies.
- **F-line and unimplemented FP** on 060: chain to the port's FPSP (emulate only). On an LC060 the Mac side is told "no FPU" through the synthetic `CPUFlag`/`HWCfgFlags` (§6.8).
- Static ROM patches are applied when the ROM object is loaded (§6.7), with the non-FPU `PACK 4`/`PACK 5` selected on 060.
- A per-application opt-out list (by COFF name or creator) for self-checksumming code.

## 5. Signals

### 5.1 Delivery

- `psig` (os 0xbbca) first updates `p_hold` (`u_sigmask[sig]` at u+0x5e8+4·sig, plus the signal unless in the NODEFER mask at u+0x5d8). It saves the pre-delivery mask in **`u_sigoldmask` (u+0x5e8)** unless the sigsuspend flag (u+0x5c4 bit 0) says it is already saved. It resets SA_RESETHAND handlers (mask at u+0x5e0, `setsigact`), then calls `sendsig(sig, sip, handler)`. A zero return makes it kill the process with SIGSEGV.
- `sendsig` wrapper: `if (AUXP(curproc)) return aux_sendsig(sig, sip, hdlr); else return __amix_sendsig(...)`.
- `aux_sendsig`:
  1. Map the AMIX signal to A/UX ([aux-syscall-translation.md](aux-syscall-translation.md) §4.1). Map the old mask (u+0x5e8, SVR4 bits) to an A/UX mask minus SIGKILL/SIGSTOP. Compute the BSD code from `sip->si_code`: SIGFPE integer divide → `KINTDIV` 2; others per a table *(to be completed from A/UX `signal.h`)*.
  2. Mac task (`ap_layer`) and sig ∉ {SIGALRM, SIGQUIT}: nested Mac interrupt level. `++layer->l_curlevel`, store it in `ap_maclevel`, sleep until this task's level is current (A/UX `sendsig` 0x10000000 / `uiswtch`). Single-task sessions pass straight through.
  3. Choose the stack: BSD with `SV_ONSTACK` and not on the stack → `ap_ss_sp`, `ap_ss_onstack = 1`.
  4. Private area first (highest addresses): the exception frame if the signal interrupted a faulting instruction (format from `u_ar0`+0x46), and FP state (`fpu_save`) **except for SIGIOT and SIGIO in Mac tasks**. Record the flag bits (bit 2 FP pushed, bits 0/1 re-raise). The layout is private; restore validates it.
  5. Frame format by `ap_compat & COMPAT_BSDSIGNALS`. SVR3 (`ssig`, handler = libc `_sigcode`) or BSD (0x40 bytes with the `move.l #150,d0; trap #15` stub and the `sigcontext`), exactly as in the translation spec §4.3. `sc_ps` = hardware SR | (vSR << 16), and the handler runs with vSR trace bits cleared (`& 0x3fff`).
  6. `u_ar0`: usp = frame, PC = handler, SR user CCR. Clear `ap_code`. Return 1.
- Disposition bookkeeping stays in AMIX: `sigvec`/`ssig` translate to the `sigaction` internals (SA_RESTART unless `SV_INTERRUPT`; `ssig` gets SA_RESETHAND + SA_NODEFER except SIGILL/SIGTRAP). The frame builder only needs the A/UX flag bits kept in `aux_proc`.

### 5.2 Return paths

- **BSD**: `trap #15` with d0 = 150, handled at the top of `aux_systrap`. Read F (usp − 4) and the `sigcontext` through F+0x0c, and restore:
  - `onstack`;
  - the mask: map to SVR4, minus KILL/STOP, then `p_hold` = mask (as `sigprocmask` SETMASK);
  - d0/d1/a0/a1 and usp/PC;
  - SR = `sc_ps & 0xc0ff` (CCR + trace only), vSR = high word & 0x700 (0xc700 if traced), Mac level.

  Leave d0/d1 as restored: no carry/errno write-back (a "no-return-value" flag in the handler entry).
- **SVR3**: `sysm68k(2, d0)` via trap #0 (A/UX 38/50). Restore flag/CCR/PC/[old usp] and d0 = saved d0, per the spec.
- A saved long exception frame (private area) is restored through AMIX's own mechanism: copy it to `u_sigsave` (`sigsave` pointer) and set `USTKRESTORE` (0x40000000) in `u_sigflag`, so `ttrap.s:stkrestore` rebuilds it. Otherwise set `USTKCLEAR` when the PC changed.
- Validation: SR supervisor/master/IPL bits never come from user memory; unknown frame formats → SIGSEGV.

### 5.3 Other signal-related points

- The virtual IPL is **not** raised by delivery (A/UX doesn't either, [aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md) §1).
- Kernel parts of the personality post **AMIX** numbers (`psignal(p, 6)` tick, 22 SIGPOLL for SIGIO, 21 SIGURG, 7 SIGEMT sound). Only frames and syscalls show A/UX numbers.
- `SIGSYS` for A/UX `nosys` (12 on both).

## 6. `/dev/uinter0` driver

### 6.1 Structure

- **A plain character driver**, `cdevsw` entry with open/close/read/write/ioctl/mmap/segmap/poll (the AMIX cdevsw has a poll slot, e.g. `scrpoll`). Fixed major, shown to A/UX code as A/UX's 16-bit dev through the personality's device table.
- The A/UX uinter is not a STREAMS device, and the Mac side uses only ioctl and select on it.
- ioctl data: the personality passes the A/UX command unchanged ([aux-syscall-translation.md](aux-syscall-translation.md) §5.3a). `uiioctl` does its own `copyin`/`copyout` using the BSD size field (bits 16–22) and direction bits, as A/UX's generic ioctl layer did. `_IO` "value" commands take the value directly.
- Files: `ui.c` (entry points, dispatch), `ui_layer.c` (layers, attach, switch/sleep, global fds), `ui_event.c` (queue, `GetOSEvent`, select), `ui_input.c`, `ui_video.c` (fake card + Slot Manager), `ui_rom.c`, `ui_pram.c`, `ui_lowmem.c` (synthetic kernel low memory), `ui_time.c` (tick, `Ticks`/`Time`).
- One device, one layer (`NLAYERS 1`), 16 tasks and 32 events, as in `uinter.h` (`UI_VERSION 5`).

`struct aux_layer` keeps the A/UX `struct layer` semantics (not its byte layout): state; event queue and mask; sleep mask and mouse rects; active task; tick state (`l_trunning`, `l_wakeme`); shm id; global file table; ROM and screen mapping records; `l_vipl`, `l_pending`, `l_curlevel`; `l_attached[16]` {proc, select set, pid, `APF_*`}; cursor state; the kernel address of the ui page.

### 6.2 Startup ioctls (the ~35 needed to reach the Finder)

From [aux-uinter-semantics.md](aux-uinter-semantics.md) "Minimum set", with the AMIX realisation:

| # | Name | Implementation on AMIX |
|---|---|---|
| 0 | GETVERSION | return 5 |
| 52 | TEST | EEXIST if layer in use |
| 7 / 8 | MAP / UNMAP | `as_fault(F_SOFTLOCK)` on user page 0x3000, record its PFN (`hat_getpfnum`/`vtop`), init `ui_interface` (`c_button` +0x458); unlock on UNMAP |
| 21 | CREATELAYER | needs MAP; create layer, attach caller as task 0, set `ap_layer` (the SMAC equivalent) |
| 46 | SHMID | store the tLOW id |
| 36 | ATTACHGFD | create the layer's global file table (§6.3) |
| 34 | PHYS_SCREENS | fill `screen[]` from the fake card(s) and map slot space (§6.7) |
| 5 / 6 | ROM / UNROM | map / unmap the ROM object (§6.7) |
| 1 / 2 | SET / CLEAR | SET: lock user 0…0x3fff (`as_fault` SOFTLOCK), record the PAs in `ap_lm_pa[]`, set `APF_UISET` (gates now reflect), map the 060 helper page; CLEAR: reverse |
| 23 | SETLAYER | make active, wake waiters |
| 9 / 10 | CURSOR / UNCURSOR | enable kernel cursor drawing at VBL (§6.6) |
| 12 | DELAY | sleep (PCATCH) until the `Ticks` value, via `l_wakeme` checked by the tick; return ticks since boot (`lbolt`) |
| 16, 18, 19, 20 | POSTEVENT, FLUSHEVENTS, GETOSEVENT, SETEVENTMASK | event queue; GETOSEVENT modes NOBLOCK/BLOCK/AVAIL/AVBLOCK, aux codes 16–18 when asked, BLOCK sleeps until event/timeout/mouse leaves the rect |
| 24 / 25 | DEVICES / UNDEVICES | route host keyboard/mouse to the layer (§6.6) |
| 26 | SETSELRECT | `l_selmouse` |
| 40 | SELECT | store the fd set, post `selEvt` when ready (poll these fds from the tick; *(uncertain: A/UX mechanism)*) |
| 51 | SET_KCHR | copy ≤ 0xc00 bytes; kernel key translation uses it |
| 69 | GETKEYS | 128-byte key map |
| 28 / 29 | READPRAM / WRITEPRAM | file-backed XPRAM (§6.8) |
| 32 | COPY_OUT | copy from the synthetic kernel low-memory image (§6.8) |
| 35 | TIMER | start the 1-tick SIGIOT timer (§6.5) |
| 42 | GETDQEL | no Mac drive-queue elements at first: return the A/UX "no more" error *(check `mac_init`'s loop end condition)* |
| 47 / 48 | VIDEO_CONTROL / STATUS | fake card driver in C (§6.7) |
| 67 | GET_PRODINFO | synthetic ProductInfo (0x34 bytes) |
| 68 | GET_INTERR_VECTORS | return 1 (A/UX's non-040 answer): host FPSP handles unimplemented FP *(verify Patch `InitExceptions` then leaves `$C0…$D8` as `fault`)* |
| 56 | GETKIFLAGS | 0 (no parity) |
| 43 / 44 / 45 | KILLMYLAYER / REBOOT / SHUTDOWN | terminate layer; `uadmin` paths as syscalls 64/65 (root only; otherwise just end the session) |

For CommandShell and COFF Mac applications add #27 HASKIDS, #30 POST_MOD, #31 FIND_EVENT, #37 ATTACHLAYER, #38 SWITCH, #39 SLEEP, #50 SYNC, #55 POST_EVTREC, #63/#64 SET/GETCOFFNAME, #65/#66 PUT/DELDQEL, #70 GETSCSIID (all "none"). Scheduling follows A/UX:

- `SWITCH(pid)`: set `l_active`, write the pid to ui page +0xff8, `psignal(SIGIOT)` a stopped target, wake it, and sleep the caller until it is active again (a negative pid means the caller leaves).
- `ATTACHLAYER`: take a free slot, map ROM and screens, install the global fds, post `attachEvt`, sleep until scheduled.
- 24-bit: `ATTACHLAYER` → EACCES for `SMAC24`-style requests.

Everything else → EINVAL (#11, #17, #33, #41, #53, #54, #57–#62, #71). The VM calls #57–#62 → noErr stubs *(check `cMemoryDispatch`'s expectations)*.

### 6.3 Global file table (`UI_ATTACHGFD`, `O_GLOBAL`)

- A/UX: fds 0–0x7f are local and **fds ≥ 0x80 index the layer's global table** (`getf` 0x1002b8a2: `u_gofile` at u+0x54a, `GNOFILE` entries). They are created with `O_GLOBAL` (0x80000000, `sys/file.h`). The Mac File Manager keeps these fds in shared FCBs, so every task must see them.
- AMIX has no shared fd tables. Design: **mirroring**. The layer holds `file_t *gfd[GNOFILE]` (with an `f_count` reference each) and a generation number. The personality's `open` with `O_GLOBAL`, and `close`/`dup2` on fd ≥ 128, update the table and bump the generation. Each task syncs lazily on entry to `aux_systrap` when `ap_gfd_gen != layer gen`: it installs or removes the same `file_t` at the same fd number in its own AMIX fd table (`setf`, `f_count++`; `closef` for removals).
- AMIX handlers therefore work unchanged. A/UX processes need `RLIMIT_NOFILE` ≥ 128 + GNOFILE (raised at exec).
- Limitation: a task that has not synced yet keeps a reference until its next syscall. Offsets are shared because the `file_t` is shared.

### 6.4 `select()` semantics

- The translator's select → poll internals (fs `poll` 0x42f2) calls `uichpoll(dev, events, anyyet, revents, phpp)`:
  - POLLIN if an event is queued that a non-blocking GETOSEVENT would return, or if the mouse (low memory `$82C`) is outside `l_selmouse`.
  - Otherwise arm `ui_seltimer` with the layer timeout (`timeout()`), which on expiry posts `waitEvt` (31) and `pollwakeup(POLLIN)`, and return the layer's pollhead.
- Event posting and mouse moves also call `pollwakeup`.

### 6.5 Tick, `Ticks` and `Time`

- **SIGIOT tick**: `UI_TIMER` arms `timeout(ui_tick, layer, 1)`. `ui_tick` wakes a `UI_DELAY` sleeper whose `l_wakeme` has passed, does `psignal(active task, 6)`, and re-arms. AMIX HZ is 60 (`CLOCK==1` in `sys/param.h`), so the tick matches A/UX. If a 50 Hz build is ever used, derive the tick from a 60 Hz accumulator.
- **Low-memory clock**: `aux_clock_hook(pcb)` (chains the previous `clock_hook`). If `curproc` is a Mac task with `layer->l_vipl == 0`, write `Ticks` ($16A, from `lbolt`), `Time` ($20C) and the alarm bits ($200/$208/$21F) into Mac low memory. These are A/UX `ui_update` 0x1008651a semantics.
  - `Time` = Unix seconds + 2082844800 + local offset *(A/UX's exact local-time source not traced; use the personality `tz`)*.
  - Catch-up on the aux return path when `l_prevtime != lbolt` (A/UX also updates from `swtch`).
- **Coherent writes to user pages from interrupt level**: write through the page's physical address (identity-mapped for RAM below 1 GB).
  - 040/060 (physically tagged): with the port's current write-through user RAM, an NC write through DTT0 is already coherent (§1a). If copyback user pages are ever enabled: `cpushl dc` on the line first, then write through the non-cacheable supervisor TT window, so no stale or modified user line survives.
  - 030: the supervisor data cache is off (`sup_cacr` 0x1019) and `cacr` 0x3919 clears the caches on every return to user.
  - The same helper `aux_upoke(pa, val, size)` serves the ui page (cursor/mouse), low memory, and the AppleTalk event ring.

### 6.6 Input and cursor (platform part)

- Amiga: the console drivers are source (`amiga/console/*`, keyboard via `aciaaintr` on the CIA). Add a hook: while a layer has `A_WANTED` and owns the display, raw key codes go to `ui_key_intr(adb_keycode, down)` (Amiga → ADB key-code table) and mouse deltas and buttons to `ui_mouse_intr(dx, dy, btn)`.
  - The kernel updates `c_mx/c_my` and posts keyDown/keyUp/autoKey (threshold and rate from the ui page) through the KCHR table, and mouseDown/mouseUp.
  - A hot key returns the display to the AMIX console.
- Cursor: drawn by the kernel at vertical blank into the frame buffer from `c_cursor`/`c_mask`/`c_data`, honouring `c_lock`/`c_newcrsr` (A/UX `uinters.s` semantics). Amiga VBL servers exist (`addvbint`, L3). A hardware-sprite cursor is an option for native chipset modes.

### 6.7 ROM, video and Slot Manager (non-Mac hosts)

- **ROM object**: `ui_rom.c` loads a ROM image file (path tunable, e.g. `/etc/aux/rom/067C`). It checks the checksum and version word at +8, and applies the §4.8 patch table on 060. The image goes into kernel pages that are **private to the session** (copy-on-open), so trap-and-patch can write them.
  - `UI_ROM(va)` → `as_map(p_as, va, size, segdev_create, …)` over the uinter device's ROM offset range (`uimmap` returns PFNs from `hat_getkpfnum`).
  - `UI_UNROM` → `as_unmap`. The TBRAM copy path (shm at 0x40800000 + `memcpy` from 0x50000000) works as well.
- **Fake NuBus video card**: one virtual slot (`$E` suggested) whose 16 MB slot space is `0xFE000000–0xFEFFFFFF`, mapped identity (VA = "NuBus address") by `UI_PHYS_SCREENS`:
  - frame buffer pages at the slot base → the host frame buffer PFNs, mapped cache-inhibited;
  - a **synthetic declaration ROM** image at the top of slot space: board sResource, one video sResource with 1/2/4/8-bpp modes and `VPBlock`s, and a driver sResource whose DRVR is only a stub, since Patch talks to the kernel through `doIoctl` anyway;
  - the image is built offline by a new tool (`tools/mkdeclrom.py`) and linked as data.
- **Slot Manager, syscall 66 `sysslotmanager`**: kernel C over the same declaration-ROM image. Copy in the 0x38-byte `SpBlock` and implement the selectors ≤ 0x30 that `Patch.067C` uses (`sReadByte/Word/Long`, `sGetcString`, `sGetBlock`, `sFindStruct`, `sReadStruct`, `sNextsRsrc`, `sRsrcInfo`, `sNextTypesRsrc`, `sCkCardStat`, `sReadDrvrName`, `sFindDevBase`, `sGetDriver`, …). Pointer results point into the mapped slot space. Empty slots → the Slot Manager's empty-slot error *(value uncertain, −300?)*. *(Open: how A/UX handled `sGetBlock`/`sGetcString`, which allocate; list the selectors Patch actually issues with `tools/callargs.py`.)*
- **Video driver**, `UI_VIDEO_CONTROL`/`STATUS`: C implementation of the csCodes the Mac side uses:
  - Control: SetMode, SetEntries, SetGray, SetDefaultMode, GrayPage, SetInterrupt;
  - Status: GetMode, GetEntries, GetPageCnt, GetPageBase, GetGray, GetInterrupt, GetBaseAddr, GetDefaultMode, GetConnection;
  - on the kernel buffer (index at +4, csCode +0x1a, csParam +0x1c; result at +4, −17 for no device).

  Palette → host CLUT. Depth changes remap the host display.
- Backends:
  - **Amiga native chipset, 1 bpp**: one hires bitplane is a 1-bpp, MSB-left linear frame buffer, the Mac format itself (colour 0 white, colour 1 black), so no copying. Chip RAM → map cache-inhibited.
  - RTG boards (8-bit chunky in Zorro space; PFNs above 0x40000000 are fine for *user* mappings).
  - Atari TT/Falcon mono later.
- **Slot interrupts** (A/UX `callSlotInt` at video VBL): not needed. The Mac side's VBL work runs from the SIGIOT tick. The fake card's own VBL duties (palette latch, cursor) run in the kernel's VBL server.

### 6.8 PRAM, clock and synthetic kernel low memory

- **PRAM**: 256 bytes XPRAM (+ classic 20 bytes), initialised with the validity signature and defaults. Read from `/etc/aux/pram` at first open (`vn_rdwr`), written back on `WRITEPRAM` (or a modified flag and flush on last close). The Mac-side AppleTalk hints (XPRAM 0xe0…) live here too.
- **Clock**: `ReadDateTime` equivalents use `hrestime`. `SetDateTime` from the Mac side → EPERM for non-root, or ignored *(decide)*.
- **Kernel low-memory image** (`UI_COPY_OUT`): a per-machine table built at `aux_init`:
  - `ROM85` $28E, `HWCfgFlags` $B22, `$D00`/`$D02`, `CPUFlag` $12F (host CPU; 060 reports 040), `KbdType` $21E (ADB extended), `UnivInfoPtr` $DD8, `BoxFlag` $CB3, `MMUType` $CB1;
  - with `ProductInfo` (`UI_GET_PRODINFO`) taken from the ROM's own universal tables for the chosen model (Quadra 700 for the 040 ROM, IIci for the IIci ROM), by a new tool or at ROM load *(values to be extracted)*.
- **SCSI/drives**: `GETSCSIID` → no usable devices; `GETDQEL` → none. HFS disk/partition pass-through is a later feature.

### 6.9 On Mac hardware

- A/UX ran ROM code in the kernel (video Open/Control/Status via `kernelAline`, slot interrupts, Slot Manager, PRAM/clock) with a Mac low-memory world at kernel VA 0 and the ROM at kernel VA 0x40800000.
- On the AMIX kernel, VA 0x40800000 is inside `kvsegmap` (0x40440000–0x48440000). Moving the kernel window is feasible, because `u`/`syssegs`/`kvsegmap`/`kvsegu` are link-time absolutes from `syms.s` resolved through about 1,070 relocations. The hard-coded remnants are the `xclosef` pointer check (os ~0x41b2/0x41be) and `pstart`'s table limit, which the port replaces anyway.
- Options:
  - (i) the same fake-card path plus native drivers per video family, real PRAM through Egret/Cuda/VIA code;
  - (ii) reproduce A/UX's kernel ROM services, which also needs the window move and a kernel Mac low-memory page.

  Recommendation: (i) first, and (ii) only if arbitrary NuBus cards with declaration-ROM drivers matter. The user-side ROM on Macs is the physical ROM mapped through `segdev` (no copy), unless trap-and-patch needs a private copy.

## 7. Sockets: two options (user decision)

Common to both:
- the calls come through `aux_systrap` (70–93, select 82);
- the socket is a **real descriptor in the A/UX process**, a stream with `sockmod` on it;
- read/write/close/dup/fork/poll/SIGPOLL are native;
- ioctls per the translation spec §7.3; `fstat` reports S_IFSOCK for `sockmod` streams;
- `SOCK_STREAM`/`SOCK_DGRAM` are swapped;
- before the transport devices exist, every call → ENETDOWN (70).

### 7a. Upcall to a user-space helper (`auxsockd`, links AMIX `libsocket`)

- Kernel side: a small clone driver `/dev/auxsock` that the daemon opens. Requests are records `{id, cred, pid, call, args, sockaddr}` read by the daemon. Replies come back by `write`/`ioctl`, and a new descriptor comes back as `{id, fd}`. The kernel `getf(fd)` in the daemon's context, takes a reference, and installs the same `file_t` in the requesting A/UX process (`falloc`-style slot + `setf`).
- Per call:
  - `socket`/`accept` → the daemon returns a new fd.
  - `bind`/`connect`/`listen`/`shutdown`/`get|setsockopt`/`get*name` → the daemon runs `libsocket` on its duplicate of the fd.
  - Blocking `connect`/`accept` → a daemon worker per blocking call; the A/UX process sleeps interruptibly on the request.
  - A signal → a cancel request (the daemon closes its duplicate or aborts).
  - `send*`/`recv*` with addresses or flags go through the daemon, or stay in the kernel for connected COTS without flags (`VOP_READ`/`VOP_WRITE`).
- `FIOASYNC`/`SIOCSPGRP` must still be done **in the A/UX process's context** (`I_SETSIG` registers the caller), so the kernel does them directly.
- Pros:
  - no TPI protocol code in the kernel; `libsocket` behaviour exactly;
  - AF_UNIX and `socketpair` come almost free (`ticotsord`);
  - daemon bugs don't panic the kernel.
- Cons:
  - two context switches plus IPC per socket-specific call;
  - worker processes for blocking calls;
  - EINTR/restart and non-blocking semantics have to be emulated across processes;
  - credential passing, needed for privileged ports and binding as the user;
  - a daemon that must be running (ENETDOWN otherwise) and is a single point of failure;
  - more kernel plumbing (fd transfer, cancel) than it first appears;
  - it still needs kernel code for SIGIO ownership.

### 7b. In-kernel TPI binding (translation spec §7.2–§7.3)

- The personality performs `libsocket`'s sequences itself: open the transport device like `ktli`'s `t_kopen` (`makespecvp` → `VOP_OPEN` → `falloc`), `strioctl(I_PUSH "sockmod")`, then `KSTR` wrappers over `strioctl`/`strdoioctl` with kernel buffers for `SI_*`/`TI_*`, and `strputmsg`/`strgetmsg` for `T_CONN_REQ`, `T_UNITDATA_REQ` and the rest.
- All socket state lives in `sockmod`, so no hidden state needs mirroring.
- About 1–1.5k lines.
- Pros:
  - native blocking, EINTR/restart and `O_NDELAY` behaviour (the process sleeps in its own STREAMS calls);
  - no daemon;
  - SIGIO/SIGURG ownership natural;
  - the lowest latency;
  - the TCP pass-through the Mac side exercises constantly (MacTCP driver in `Patch.067C`) stays in one process.
- Cons:
  - protocol-level code in the kernel, where a bug is a panic;
  - it depends on internal STREAMS entry points (`strputmsg`/`strgetmsg` with kernel buffers: check the `ktli` callers for the calling convention);
  - AF_UNIX and access rights deferred (EOPNOTSUPP / EPROTONOSUPPORT) or done later by the same mechanism.

Middle path: 7b for AF_INET plus 7a for AF_UNIX only.

## 8. AppleTalk

- **A/UX-facing layer, always ours** (spec in [aux-appletalk-interface.md](aux-appletalk-interface.md) §8):
  - the clone device `/dev/appletalk/ddp/socket` and the control device `/dev/appletalk/lap/ethertalk0/control`;
  - modules `at_sig`, `at_atp`, `adsp`;
  - the `I_STR` commands 0xcb01–0xcb06, 0xca01–0xca05, 0xd601/2, 0xd001, 0x7c01/03/0d/0e/0f/16, 0xd4f1–0xd4fd;
  - one datagram per `read` (RMSGD);
  - syscall 167 `atp_control` (Mac tasks only);
  - the **event ring**: the registered 4 KB table (`DDP_IOC_[LRU]STATUS_TABLE`) is locked (`as_fault` SOFTLOCK), its PA recorded, and written with `aux_upoke` (§6.5 coherency). SIGIO (AMIX 22) to the owner on empty → non-empty.
- **Provider interface** below it, a kernel-internal ops vector so the stack can be swapped:

```c
struct at_provider {
    int  (*ifup)(char *ifname, at_elap_cfg_t *hint);
    int  (*ifdown)(void);
    int  (*getcfg)(at_ddp_cfg_t *);
    int  (*setzone)(at_nvestr_t *);
    int  (*bind)(int sock, void *cookie);
    void (*unbind)(int sock);
    int  (*output)(mblk_t *ddp_dgram);            /* extended DDP header + data */
    /* provider -> shim */
    void (*input)(void *cookie, mblk_t *dgram);
    int  (*zip_getmyzone)(at_nvestr_t *);
    /* ATP/ADSP either in the shim (on DDP) or delegated to the provider */
};
```

- Candidate providers (open decision):
  - (1) A/UX's own AppleTalk STREAMS objects (ELAP/AARP/DDP/RTMP/ZIP/NBP/ATP/ADSP) behind a DDI shim. Different STREAMS, queue and DLPI ABIs; probably costly.
  - (2) BSD-licensed kernel DDP/AARP (NetBSD netatalk kernel part) ported to STREAMS over AMIX DLPI Ethernet, with our ATP/ADSP.
  - (3) A user-space provider daemon (netatalk-derived) behind a STREAMS multiplexor, with the kernel only moving DDP datagrams.

  ATP/ADSP location follows the choice. The shim's interface to the Mac side does not change.
- Needed from the host in every case: raw Ethernet with SNAP/802.2 (AppleTalk 0x809B, AARP 0x80F3) and multicast. The AMIX `aen` driver (cdevsw 18) must support them *(verify)*.

## 9. Layout, build and staged milestones

### 9.1 Object layout

New tree `aux/` beside `amiga/` in the unified kernel (K&R C, per the tool style):

| Object | Contents |
|---|---|
| `aux/auxgate.s` | vector gates, A-line/trace fast paths, 060 helper page image |
| `aux/auxproc.c` | `aux_proc`, `ev_config`/`ev_fork`/`ev_exec`/`ev_exit`/`ev_istrap`, `aux_init` (saves chain vectors) |
| `aux/auxexec.c` | `aux_coffexec`, A/UX header/`.lib` parsing, AppleSingle entry (later) |
| `aux/auxvm.c` | `valid_usr_range` wrapper, `segdev` helpers, `aux_upoke`, page locking |
| `aux/auxsys.c`, `auxsysent.c` | `aux_systrap`, the A/UX call table (generated from the spec tables), errno maps |
| `aux/auxconv.c`, `auxioctl.c` | stat/dirent/statfs/ipc/termio/ioctl conversion, device-number map |
| `aux/auxsig.c` | `sendsig` wrapper, `aux_sendsig`, `sigcleanup`, `sysm68k(2)`, `sigvec`/`ssig`/masks, `fsig` override |
| `aux/auxcpu.c` | fault reflection, `aux_priv`, 060 trap-and-patch |
| `aux/auxmisc.c` | itimers, time of day, `xstat`/`setxinfo` stubs, `fidop`/`csop` kmsgq, asio, `sema_*`, `memlock`, `sysm68k` subcommands |
| `aux/auxnet.c` (7b) or `aux/auxsock.c` + `auxsockd` (7a) | sockets |
| `aux/uinter/*.c` | §6 |
| `aux/atalk/*.c` | §8 shim |
| `master.d/aux.c` + `master.d/aux` | tunables: `aux_coff_default`, GNOFILE, ROM/PRAM paths, emulated model, 060 patching on/off |

### 9.2 Build integration with the AMIX 040/060 port

- The port patches the linked 2.1c `stand/unix` with `relink-040.sh` (Linux, `m68k-cbm-sysv4-gcc`, `m68k-linux-gnu-objcopy`, `ld -r`, then fail-closed Python patchers). Add an `aux` stage in the same style:
  1. Add to the port's existing `objcopy` call: `--weaken-symbol` + `--add-symbol X_orig=.text:<off>,function,global` for W symbols (`sendsig`, `valid_usr_range`), `--weaken-symbol fsig`, and `--weaken-symbol` for `ev_config`/`ev_fork`/`ev_exec`/`ev_exit`/`ev_istrap`.
  2. Add our objects to the `ld -r` list.
  3. Run a new `patch_aux_vectors.py` after `patch_isp_vec61.py`: retarget slots 3–10, 32 and 47 from `nullvect`, and slot 61 from `isp61_vec`. It asserts the expected current targets.
  4. Run `patch_aux_tables.py`: `execsw[0].exec_func` → `aux_coffexec`; free `cdevsw` rows → uinter and AppleTalk entry points (precedent `patch_va2000_cdevsw.py`); `aux_init` via an `io_init`/`init_tbl` slot.
  5. **Port change** in `pstart040.s`: ITT0 0x003fa000, DTT0 0x003fa060 (supervisor-only). Plus the `vtop040.s` extension for A/UX user buffers below 0x80000000.
  6. Console input hook: the Amiga console drivers are inside `stand/unix` too, so hook their key/mouse entry points by W, not source.
- Offsets in this document are for the stock 2.1 kit. Like the port, derive them from `stand/unix`'s symbols at build time and assert them; never hard-code them.
- The gates copy `nullvect_orig`'s entry sequence (`moveml`, `sup_cacr` → CACR). The port keeps this sequence; the CACR values are the port's.
- Regression guard: build the same kernel with `aux/` and without, and run AMIX native test programs. The only native-path change is the syscall gate.

### 9.3 Milestones (each with a pass criterion)

| # | Milestone | Needs | Pass criterion |
|---|---|---|---|
| M0 | Hooks linked, inert | §9.2, gates with the personality always off | kernel boots; native syscall timing within noise of stock |
| M1 | Static A/UX COFF | `aux_coffexec`, `aux_systrap`, errno, core N/C calls, SVR3 frames | A/UX `/bin/sh` runs scripts; `echo`, `cat`, `ls` (static ones) |
| M2 | Shared-library A/UX programs, BSD signals | `.lib` loader, `valid_usr_range`, stat/dirent/termio conversion, `sigvec`/`sigcleanup`, `setcompat` | A/UX `ls -l`, `more`, `vi` on a pty; a `sigvec` test program |
| M3 | `startmac` enters Mac code | uinter setup ioctls (0, 52, 7, 8, 21, 46, 36, 34, 5, 6, 1), ROM object, A-line fast path, privilege emulation, low-memory tables | `TBVERBOSE`/`TBWARN` output past `doDispatch`; `doOSModules` opens the System file |
| M4 | **Finder desktop (first target)** | fake video card + Slot Manager, events/keyboard/mouse/cursor, SIGIOT tick + virtual IPL, PRAM, `xstat` stub (AppleDouble `%` files), `fidd` running, `Ticks`/`Time` | System 7.0.1 Finder on a 1-bpp Amiga screen, with Unix volumes browsable |
| M5 | CommandShell, COFF Mac apps | ATTACHLAYER/SWITCH/SLEEP/SYNC, global fds, `UI_SELECT`, ptys (`TIOCPKT` if needed) | CommandShell window runs `ls`; `launch` a COFF Mac tool |
| M6 | TCP/IP | §7 (chosen option), `select` | MacTCP application (e.g. telnet) reaches a host |
| M7 | 060 speed | $F4 trap-and-patch, ROM patch table, helper page | Finder redraw on A4000/060 without per-multiply exceptions (count them) |
| M8 | Serial, sound, colour | tty ioctls, sound shim, 8-bpp RTG backend | |
| M9 | AppleTalk | §8 with the chosen provider | Chooser sees an AFP server |

## 10. Risks and open questions

**Risks**

1. **Port interaction (§1a).** The per-process vector approach fits: slots are retargeted by relocation, as the port does, and entry and exit go through `nullvect`/`ureturn`. Remaining risks:
   - ITT0/DTT0 must become supervisor-only, or every quadrant-0 A/UX access silently hits physical memory. The port has not yet made this change (its N1 plan).
   - `vtop040.s` and the HAT have never seen user VAs below 0x80000000.
   - The measured cost of exception-path code on the port (7.74 µs per syscall for its probe wrapper) means the gate needs a benchmark (fallback: VBR switch in the port's `resume`).
   - The port is a moving target: its patchers assert exact relocation targets, so our patchers must run after theirs and assert theirs in turn.
2. **`p_evpdp` reuse.** Safe as far as os/fs/vm/io/disp/exec show. The remaining objects are not scanned. The fallback is the `pid_prslot` table plus a p_flag bit.
3. **Signal holding via `fsig` only.** Spurious wake-ups of sleepers by held signals rely on SVR4 sleepers re-checking their condition. An unusual sleeper could see an unexpected early return *(audit the `sleep` PCATCH path)*.
4. **Performance.** C-level privilege emulation may be hot (ROM `ori #$700,sr` sections). The A-line fast path must stay assembly. Measure before optimising.
5. **Exception frame formats seen by Mac code.** Reflected bus/address-error frames on a 060 host carry 060 formats that the 040 ROM and applications don't know. `rte` emulation handles formats 0/2/3/9 only, as A/UX does.
6. **Cache coherency.** Kernel writes into user pages (low memory, ui page, AppleTalk ring, trap-and-patch) on copyback caches. The design relies on `cpushl` before physical writes. Any path that misses it gives rare, hard-to-find corruption.
7. **Global-fd mirroring** is an approximation of A/UX's shared table: close visibility is delayed until each task's next syscall.
8. **Stack top.** Keeping 0xc0800000 assumes no Mac-side code depends on A/UX's 0x40000000.
9. **All A/UX COFF are claimed by the personality**: a genuine SVR3 68k COFF without `.lib` would be misrun. The tunable only mitigates this.
10. **Slot Manager emulation completeness.** Pointer-returning selectors and the error codes are guesses until Patch's call sites are listed.
11. **Size of the translator.** It is large (≈170 calls plus ioctls). M1–M2 need only a subset.

**Open questions**

- A/UX's handling of A-line and trap #2 for non-Mac A/UX processes (signal numbers).
- A/UX `sendsig` level-nesting details (`uiswtch` loop, `SCHKLVL`) for multi-task layers.
- Exact `UI_SELECT` wake mechanism; `UI_GETDQEL` end-of-list convention; `cMemoryDispatch` expectations for #57–#62.
- `Time` ($20C) local-time source; `SetDateTime` policy.
- ProductInfo/low-memory values per emulated model (to extract from the ROM dumps); whether `BoxFlag` rewriting in `doVariables` needs a particular model.
- Whether the HAT and `as_*` accept user segments above `UVEND` (0xf1000000) and at 0xFFFF8000.
- `hrtsys` never reached from A/UX processes (`ev_config` side effect).
- Sockets 7a vs 7b; AppleTalk provider; loadable modules (would let `aux/` load without a relink).
- Mac-hardware path (§6.9): fake card plus native drivers, or A/UX-style kernel ROM services with a moved kernel window.
