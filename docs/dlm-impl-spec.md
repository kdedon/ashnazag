# DLM implementation specification

Implementation spec for SVR4.2-style dynamically loadable modules (DLM) in our kernel: the AMIX 2.1c relink image plus the 040/060 port plus a platform layer (Mac first), built by weakening and overlaying symbols on the linked image (`kernel/mac/relink-mac.sh` style, toolchain `m68k-cbm-sysv4`, GCC 2.7.2.3 / binutils 2.8.1).

Inputs merged:

- **[C]** `docs/dlm-spec.md`: functional spec from public UnixWare documentation.
- **[S]**: behavioural spec of SVR4.2.
- Facts about our image come from `kernel/build/unix-mac` (the `ld -r` image) and `unix-mac.elf`, from the kit's `master.d/*.c`, and from the AMIX headers. They are marked **[I]**.

Rule for conflicts: **[S] wins on behaviour** because it describes the system that user programs and module writers were written against, while [C] guesses where the manuals are silent. [S] loses where it marks a defect (we apply its recommendation), and where the SVR4.0/68k target or our overlay build forces a different mechanism. Each such case is listed in §13.

---

## 1. Overview

```
 user:  modadmin  idmodreg  idmodload  moddaemon            (libmod.a stubs, trap #0)
          |          |          |          |
 ------ sysent[64..69] (written at boot by dlm_init) -----------------------------
 kernel DLM core (static): loader, relocator, symbol tables, module list,
        registry, switch linkages, trampolines, auto-unload, halt
          |                 |                    |                  |
   cdevsw/bdevsw rows   fmodsw (enlarged)    vfssw (enlarged)   execsw (enlarged)
   (empty rows = pool)  + STREAMS pool       + vfsops tramps    + exec tramps
          |                                                         sysent[70..77,82,83,105,140]
   hooksw[] (named hook pointers for the A/UX personality)
```

Loaded code lives in `kmem_alloc` memory; its symbols join a dynamic table that starts with the static kernel's embedded symbol table (§5).

---

## 2. System calls

All handlers use the AMIX `sysent` convention: `int h(void *uap, rval_t *rvp)`, return 0 or an errno; results in `rvp->r_val1`. **[I]** `sysent` has 142 entries (`sysentsize` = 0x8e), 8 bytes each (`sy_narg`, `sy_flags`, pad, `sy_call`); slots 0 (indirect), 56, 64–77, 82, 83, 105, 140 are `nosys`.

### 2.1 Numbers

| No. | Call | narg | sy_flags |
|---|---|---|---|
| 64 | `modload(const char *path)` | 1 | `SETJUMP` |
| 65 | `moduload(int modid)` | 1 | `SETJUMP` |
| 66 | `modpath(const char *path)` | 1 | `SETJUMP` |
| 67 | `modstat(int modid, struct modstatus *st, int next)` | 3 | `SETJUMP` |
| 68 | `modadm(int type, int cmd, void *arg)` | 3 | `SETJUMP` |
| 69 | `getksym(char *name, unsigned long *value, unsigned long *info)` | 3 | `SETJUMP` |
| 70–77, 82, 83, 105, 140 | reserved for syscall modules (12 slots, §9.2) | per registration | per registration |

Order follows [S] Q8. Why these slots:

- They are `nosys` in AMIX, so `sysent` needs no enlargement and no relink-time table surgery. `dlm_init` writes the six entries at boot (`sysent` is in `.data`).
- **A/UX check**: `docs/aux-syscall-translation.md` §1 routes every A/UX `trap #0`/`#15` through the personality's own table (`auxsysent`, A/UX numbering), and nothing passes through by number. N-class rows call AMIX handlers by symbol, and none of them is a `nosys` slot. No collision is possible. A/UX processes never see 64–77.
- **To verify before freezing**: ASV's `sysent` for 64–77, 82, 83, 105, 140, once ASV binaries are expected to run on this kernel.
- **Why `SETJUMP` on all six:** without it a signal during an interruptible sleep would `longjmp` to a stale `u.u_qsav`. Each handler also runs its own guard (§8.6), so partial state is unwound.
- `sysent` is never grown. `getudev` hands out pseudo-device majors starting at `max(cdevcnt, bdevcnt)` **[I]**, so growing the device switches would renumber fifofs, namefs, procfs and specfs devices. That is a second reason to keep all tables at their current device sizes.

User stubs live in `libmod.a` (one `.s` per call): `movel #n,%d0; trap #0`, carry set means error. They copy the sequence of an AMIX `libc.a` stub, including its retry on `ERESTART` (91).

### 2.2 Errno values

AMIX's highest errno is `ESTALE` 151 **[I]**. The five names from [S] Q8:

| Name | Value | Meaning |
|---|---|---|
| `ENOLOAD` | 164 | a required module could not be loaded |
| `ERELOC` | 165 | object-file, symbol or relocation error |
| `ENOMATCH` | 166 | symbol not found (`getksym`) |
| `EBADVER` | 167 | wrapper revision mismatch |
| `ECONFIG` | 168 | configured kernel resource (slots) exhausted |

- The values are implementation-defined ([S] Q8). No i386 value is copied.
- A/UX processes never receive them: the personality's SVR4→A/UX table maps unknown values to `EIO` (translation §3).
- `libc`'s `sys_errlist` is not changed. `modadmin` and the other tools carry their own messages for 164–168.
- 152–156 are avoided: ASV uses 152, 153 and 156.

### 2.3 Behaviour per call

Privilege: `suser(u.u_cred)` for all except `getksym`, else `EPERM` ([S] §2.1 item 9). Every call returns `ENOSYS` while the DLM is uninitialised (no valid kernel symbol table, §5).

**`modload(path)`**
- Module name is the last component of `path`; longer than 14 characters → `ENAMETOOLONG`. The path is copied in with `copyinstr` (`MAXPATHLEN` 1024).
- Absolute path: used as is. Otherwise each `modpath` element in turn; the error from the last element tried is returned.
- Loads through §6 with the caller's credentials and root.
- If the module is already loaded: returns its id and sets the demand mark, even if it was auto-loaded.
- Success: sets the demand mark; `r_val1` = id.
- Errors, per [S] §2.1 item 1 with the recommended fixes:

| Condition | errno |
|---|---|
| not ELF, not `ELFCLASS32`/`ELFDATA2MSB`/`EM_68K`/`ET_REL` | `EINVAL` |
| read error on ELF header or section headers | that error |
| no allocated type-13 section, or one smaller than 4 bytes | `EINVAL` |
| name of a statically configured module | `EINVAL` |
| any dependency failed to load; dependency cycle; depth > 8 | `EINVAL` |
| read error on section contents, symtab, strtab or relocations; bad symtab; bad section index; undefined non-weak symbol; `SHT_REL` section; unsupported or overflowing relocation; relocation not against the module's single symtab | `ERELOC` |
| wrapper revision ≠ `MODREV` | `EBADVER` |
| image larger than `dlm_maximage` (tunable, default 2 MB) | `ENOMEM` (ours; allocation otherwise sleeps) |
| `_load` error | passed through |
| linkage install error | passed through (`EINVAL`, `ECONFIG`, `EEXIST`) |
| stub patching failure (§11) | `ERELOC` |
| interrupted by a signal while loading | `EINTR` (after full cleanup) |

- Undefined symbol names go to the message buffer only (`cmn_err(CE_NOTE, "!...")`), as [S] says. Tunable `dlm_verbose` = 1 also prints them on the console.

**`moduload(modid)`** ([S] §2.1 items 4–5)
- `modid` > 0:
  - no module loaded at all, or id unknown → `EINVAL`;
  - clear the demand mark first (so a failed demand unload leaves the module eligible for auto-unload);
  - loading, unloading or busy → `EBUSY`;
  - else the result of §7.3.
- `modid` 0:
  - `EINVAL` if nothing is loaded;
  - otherwise walk the list, clearing each demand mark and trying §7.3; restart from the head after every success;
  - return 0 if at least one module was unloaded, else `EBUSY`.

**`modstat(modid, st, next)`**
- `next` = 0: the module with that id; missing or in transition → `EINVAL`.
- `next` ≠ 0: the settled module with the smallest id ≥ `modid`. Modules in transition are **skipped** (fix of [S] Q9), so `modadmin -S` never ends early; none → `EINVAL`.
- Copy-out fault → `EFAULT` (fix of [S] §2.1 item 6).

**`modpath(path)`** ([S] §2.1 item 7)
- `NULL` resets to the default `/etc/conf/mod.d`.
- Otherwise the string must start with `/`, and every element after a `:` or a blank must also start with `/`; else `EINVAL`. Too long for `copyinstr` → `ENAMETOOLONG`.
- The new string is prepended, so the default stays last. The total path is limited to 1024 bytes (`EINVAL` if exceeded).
- Missing directories are skipped at load time.

**`getksym(name, value, info)`** ([S] §2.1 item 8; [S] wins over [C])
- The direction is chosen by the input word `*value`:
  - `*value` == 0: look up `name` (copied in, at most `MAXSYMNMLEN` = 256 bytes including NUL, else `ENAMETOOLONG`). Return the symbol's value, and in `*info` its ELF type.
  - `*value` ≠ 0: find the image containing the address (the static kernel `[kh_lo, kh_hi)`, or a module's image). Find the greatest symbol ≤ address in that image's table, write its name back into `name` (truncated to `MAXSYMNMLEN`), and return the offset in `*info`.
- Search order: static kernel, then modules in load order, only those whose symbol table is usable.
- Not found → `ENOMATCH`. Bad pointer → `EFAULT`. No privilege needed.
- During the search each module is marked in-call (§8.5), not held.

**`modadm(type, cmd, arg)`**: registration (§3.1). One extension: `cmd` `MOD_C_AUTOUNLD` runs the auto-unload daemon (§7.4).

---

## 3. Registration and slots

### 3.1 `modadm` registration ([S] Q2, §2 item 17)

`modadm(type, MOD_C_MREG, struct mod_mreg *)`, with:

```c
struct mod_mreg {
	char	md_modname[MODMAXNAMELEN];	/* 15: file name, 14 + NUL */
	caddr_t	md_typedata;			/* int or pointer, per type */
};
```

| type | Code | `md_typedata` | Effect |
|---|---|---|---|
| `MOD_TY_NONE` | 0 | – | `EINVAL` for `MOD_C_MREG` |
| `MOD_TY_CDEV` | 1 | major (int) | char row → non-STREAMS placeholder |
| `MOD_TY_BDEV` | 2 | major (int) | block row → placeholder |
| `MOD_TY_STR` | 3 | pointer to the `I_PUSH` name (≤ 8 chars), or 0 = use `md_modname` | append an `fmodsw` entry |
| `MOD_TY_FS` | 4 | pointer to the fs type name | append a `vfssw` entry |
| `MOD_TY_SDEV` | 5 | major (int) | char row → STREAMS placeholder |
| `MOD_TY_MISC` | 6 | – | no-op, succeeds |
| `MOD_TY_EXEC` | 7 (ours) | pointer to `struct mod_execreg { short er_magic; short er_flags; }` | exec slot (§9.1) |
| `MOD_TY_SYS` | 8 (ours) | pointer to `struct mod_sysreg { int sr_num; char sr_narg; char sr_flags; }` | syscall slot (§9.2) |

Rules:

- Type codes are range-checked correctly: 0 ≤ type ≤ 8. [S]'s off-by-one is not reproduced.
- Registration is additive and idempotent. Re-registering a name/key pair that already holds that module's placeholder, *or that module loaded*, succeeds. This is [S]'s recommendation; it removes the loaded-STREAMS `EEXIST` quirk.
- A row, name or number that belongs to a static entry, or to another module's registration → `EEXIST`.
- No free slot, or a major ≥ table size (char and block alike) → `ECONFIG`.
- A driver with several majors registers once per major. A module with several magics registers once per magic.
- Re-registering a char major may switch it between non-STREAMS and STREAMS form while it is not loaded. If loaded → `EBUSY`.
- A STREAMS module's `I_PUSH` name may differ from its file name. [S] N2 allows honouring the datum; this allows 14-character file names with 8-character STREAMS names.
- There is no unregister ([S] Q2). A reboot clears the registry.
- Needs privilege (`EPERM`).
- Registration never loads anything.

### 3.2 Switch tables: what exists, what grows

**[I]** Sizes and uses in the image:

| Table | Rows | Count variable | References (all by symbol, none section-relative) |
|---|---|---|---|
| `cdevsw` | 70 | `cdevcnt` = 70 | 44 text relocations |
| `bdevsw` | 32 | `bdevcnt` = 32 | 21 |
| `fmodsw` | 11 | `fmodcnt` = 11 | `findmod`, `qattach`, `strioctl`, `getmid`, `getadmin`, `apush_iocdata` |
| `vfssw` | 12 | `nfstype` = 12 | `mount`, `dounmount`, `vfs_getvfssw`, `vfsinit`, `sysfs*`, `*statvfs`, port `sync` |
| `execsw` | 3 | `nexectype` = 3 | `gexec` (+ one port debug unit) |
| `sysent` | 142 | `sysentsize` | `systrap` only |

The scan of `.rela.text`/`.rela.data` found no reference into these tables through a section symbol, so replacing a table's definition rebinds every user.

**Device switches: no growth.**
- The DLM pool is every row that is *empty* at registration time: all routine entries `nodev`, `d_str`/`d_ttys` NULL, flag pointing to zero.
- Rows filled at boot (e.g. the RAM disk's `cdevsw[42]` from `config()`) or by table patches (A/UX `uinter`) are not empty, so they give `EEXIST`.
- In the current Mac image this leaves char 43, 47–49, 51–69 and block 0–15, 21–31 (minus rows the platform takes later).
- `CDEV_RESERVE`/`BDEV_RESERVE` are therefore not tunables. The build tool `dlmslots` lists the empty rows of the final image, and `mkmod` assigns loadable majors only from that list.

**`fmodsw`, `vfssw`, `execsw`: enlarged by override.**
- For each, `relink` adds a global alias `__amix_<tab>` at the stock address (`objcopy --add-symbol`), weakens `<tab>`, and links `dlmconf.o`, which defines `<tab>[static + RESERVE]`.
- `dlm_init` (§8.1) copies the stock rows from the alias before any user runs: `startup`→`dmainit` precedes `init_tbl` (`strinit`, `vfsinit`) and the first exec.
- The count variables keep meaning "rows in use". Registration appends and raises them, as SVR4.2 does for `vfssw` ([S] §5 item 1). So `findmod`, `vfs_getvfssw`, `sysfs`, `sync`, `gexec` and the STREAMS admin scans see registered entries unchanged, and no scan ever sees an empty row.
- Capacities (compile-time, in `dlmconf.c`): `FMOD_RESERVE` 16, `VFS_RESERVE` 8, `EXEC_RESERVE` 8.
- A panic before `dlm_init` finds the new `vfssw` zeroed: the port's `sync` skips NULL `vsw_vfsops`, and `nfstype` rows 1–11 have NULL ops at that point in stock anyway.
- Rows are never removed, so indices stay stable. This matters because `sad` and `qattach` work by `fmodsw` index and `vfs_fstype` stores the `vfssw` index.

**Placeholder states per slot kind** (the DLM keeps one `struct dlm_slot` per claimed slot: registered name, kind, owner module or NULL, pool index):

| Kind | Placeholder (registered, not loaded) | Installed |
|---|---|---|
| char, non-STREAMS | `d_open` = `dlm_cdev_open`, `d_close` = `dlm_cdev_close`, rest `nodev`, `d_flag` → 0 | module's row, but `d_open`/`d_close` stay the trampolines (real ones saved in the slot) |
| block | `d_open`/`d_close` trampolines, `d_strategy`/`d_print` `nodev`, `d_size` NULL, `d_flag` → 0 | as char |
| char, STREAMS | routines `nodev`, `d_str` = pool entry's placeholder streamtab | `d_str` = pool entry's wrapper streamtab (§4.2) |
| `fmodsw` | name, `f_str` = pool placeholder streamtab, `f_flag` → 0 | `f_str` = pool wrapper streamtab, `f_flag` = module's |
| `vfssw` | name, `vsw_init` NULL, `vsw_vfsops` = `&dlm_vfsops[i]` (trampolines), `vsw_flag` 0 | unchanged pointer; `vsw_flag` = module's; real ops saved in slot |
| `execsw` | `exec_magic` → slot's short, `exec_func` = `dlm_exec_K`, `exec_core` = `nodev` | unchanged; real functions saved |
| `sysent` | `sy_narg`/`sy_flags` from registration, `sy_call` = `dlm_sys_K` | unchanged; real handler saved |

- Old-style (`D_OLD`) drivers and STREAMS modules are **not loadable**: install fails with `EINVAL`. SVR4.2 applied its compatibility wrapping per slot; we don't ([S] §5 item 5). Converted A/UX STREAMS code (AppleTalk) must present SVR4 entry points through its shim.
- Slot updates (several words) run at `splhi`, so interrupt-level STREAMS code never sees a half-written entry.

### 3.3 Owner and per-queue side tables

- **Slot owners**: `struct dlm_slot` holds the owner. Owner tables sized like the switches (`dlm_cslot[70]`, `dlm_bslot[32]`, `dlm_fslot[11+16]`, `dlm_vslot[12+8]`, `dlm_xslot[3+8]`, `dlm_sslot[12]`) map a row index to its slot record (NULL for static rows).
- **Per-queue side table** ([S] §5.1: SVR4.0 queues have no spare field): a hash (64 buckets) keyed by read-queue address. Each entry is `{queue_t *rq; struct dlm_mod *mod; next}`, from `kmem_alloc`.
  - Inserted after the module's or driver's real open succeeds for a queue pair not yet in the table. [S]'s third-pass note says the owner is recorded after the real open.
  - Removed, with one `mod_rele`, when the real close returns.

---

## 4. Reference counting and interposition

### 4.1 Model ([S] §3.6, with fixes)

- **Two persistent counts** per module: `refs` (users) and `deps` (loaded modules that list it as a direct dependency).
- **One transient count**, `incall`: code currently executing in the module through a forwarding trampoline that must not look like a use (e.g. `sync` through an unmounted fs type). It blocks unload but never touches the candidate list.
- **Busy** = `refs` ‖ `deps` ‖ `incall` ‖ profiler lock.
- **Hold**: `refs++`; if it rose from 0, unlink from the candidate list.
- **Release**: `refs--`; if `refs` = 0 and `deps` = 0, stamp `lbolt` and append to the candidate list.
- **Dependents**: gaining a dependent also unlinks from the candidate list. Appending is a no-op if already listed. This fixes [S] N4.
- **Demand mark**: never auto-unloaded while set ([S] §2 item 22).

### 4.2 Where holds come from: trampolines, not core edits

[S] §5 puts holds into the SVR4.0 core (specfs, clone, STREAMS attach/detach, mount). **[I]** In our image `spec_open` and `spec_close` are **local** (`t`), so they cannot be weakened. Everything else would need a wrap of binary code. All holds therefore happen in DLM-owned entry points installed in the slots. Only `dounmount` is wrapped. The table lists each [S] hook site:

| [S] hook site | AMIX symbol (binding) | Our mechanism | Behaviour |
|---|---|---|---|
| special-file open, char/block non-STREAMS | `spec_open` (t, local) | slot trampoline `dlm_cdev_open` / `dlm_bdev_open` | auto-load if the slot is a placeholder; call the real open; on success add `(dev_after_open, otyp)` to the slot's open set, holding once per new key; `OTYP_LYR`: hold per call |
| special-file close | `spec_close` (t), `device_close` (T) | `dlm_cdev_close` / `dlm_bdev_close` | call the real close; on return remove `(dev, otyp)` and release; `OTYP_LYR`: release per call; unknown key → count in `dlm_stale_close`, no release |
| controlling-terminal node | specfs | none needed | the driver's own close semantics are what we count |
| clone open | `clnopen` (T) | none: it calls `setq` + `qi_qopen` of `cdevsw[m].d_str`, which is our pool streamtab | counted per queue pair; the [S] "different major" caveat cannot arise |
| STREAMS driver open/close | `stropen`/`strclose` (T) | pool wrapper `qinit` on the read side: `qi_qopen` = `dlm_str_open`, `qi_qclose` = `dlm_str_close`; other fields copied from the module; write side = module's own `qinit` | open: real open, then side-table insert and hold if new; reopen: no new hold; close: real close, then remove and release |
| STREAMS module attach/detach | `qattach`/`qdetach` (T) | same pool wrapper `qinit` (autopush goes through `qattach` too) | same |
| STREAMS module lookup | `findmod` (T) | none: registration appends to `fmodsw`, `fmodcnt` grows | registered names resolve to their stable index; `I_LOOK`/`I_FIND` compare `q_qinfo` with `st_wrinit`, which is the module's own, so they still match |
| mount | `mount` (T) | `vsw_vfsops` = per-slot trampoline vector | `vfs_mount` trampoline: auto-load if needed (failure → `ENOLOAD`, [S] §2 item 21); set `vfsp->vfs_op` to the real ops; hold; call real mount; release on failure. Remount never reaches it, because `vfs_op` is already real |
| unmount | `dounmount` (T; callers `umount`, `dis_vfs`, `vn_remove`, `nm_unmountall`, RFS) | **W-wrap** | read `vfsp->vfs_fstype` first; call `__amix_dounmount`; on 0, if that `vfssw` row has an owner, release |
| swapper auto-unload | `sched` (T, endless loop) | not wrappable → daemon (§7.4) | |
| halt | `dhalt` (T; callers `mdboot`, `kbintr`) | **W-wrap** | call each loaded module's halt routine in load order, then `__amix_dhalt` |
| DLM init in `main` | `dmainit` (T, empty stub in `master.d/stubs.c`, called at `startup`+0x70) | **O-override** | `dlm_init()`, then weak `plat_dmainit()` if a platform defines one |
| profiler | `prfopen`/`prfclose` (T) | **W-wrap** both (§7.5) | |
| exec dispatch | `gexec` (T) | none: `execsw` trampolines (§9.1) | |
| syscalls | `systrap` (T) | none: `sysent` trampolines (§9.2) | |

Why open sets and not per-open counts:
- Per-file-close balancing is impossible without editing specfs, because `d_close` runs only at the last close.
- The open set mirrors exactly what the driver sees, so the module is busy exactly while the driver has something open. That is the documented UnixWare condition: a driver becomes a candidate at the last close of all its devices.
- **To verify** by disassembling `spec_open`/`spec_close`/`device_close`: whether a block device opened as `OTYP_BLK` and `OTYP_MNT` gets one `d_close` or one per `otyp`. The `(dev, otyp)` key is the safe default: if SVR4.0 collapses types, a reference leaks (the module stays loaded and `moduload` says `EBUSY`), which is never a premature release. If collapsed, key by `dev` alone. `dlm_stale_close` > 0 on a running system signals this case.

Placeholder open when the module is not loaded:

- **char/block**
  1. Auto-load by slot name (§6.1).
  2. If the row is still the placeholder (the module was built for other majors), fail with `ENXIO` ([S] §2 item 21, block check included).
  3. Otherwise proceed as the installed trampoline.
- **STREAMS (driver, clone or module)**
  1. Find the pool index from `q->q_qinfo`.
  2. Auto-load.
  3. If the slot is still the placeholder, fail with `ENXIO`.
  4. Otherwise `setq(q, wrapper_rinit, module_wrinit)` (**[I]** `setq` is global) and continue into `dlm_str_open` with the same arguments.

**STREAMS pool**: `STR_POOL` = 32 entries (fmodsw reserve 16 + STREAMS-driver slots 16). Each entry has:
- a placeholder streamtab + read `qinit` + `module_info` (`mi_idname` → slot name, `mi_idnum` 0);
- a wrapper streamtab + read `qinit`, filled at install.

The read `qinit` address identifies the entry. The mux fields (`st_muxrinit`, `st_muxwinit`) are copied unchanged.

### 4.3 Trampoline guard

Every trampoline that holds across a call that may sleep (device and STREAMS opens, mount, exec, syscalls) runs `DLM_GUARD`:

1. save `u.u_qsav`;
2. `setjmp(&u.u_qsav)`;
3. on a `longjmp` back: undo its own hold or in-call count, restore the saved label, and `longjmp` to it.

This keeps counts balanced when a module sleeps without `PCATCH` and a signal arrives.

---

## 5. Kernel symbol table

**Choice: embedded.** The kernel carries its global symbols in a reserved `.data` block that a post-link tool fills. Loading from `/stand/unix` at boot was rejected:
- the Mac kernel boots from a Mac file (A/UX Startup) or from QEMU `-kernel`, so no Unix path is guaranteed to be the running image;
- a mismatch would silently corrupt modules.

[S] §5 item 9 confirms SVR4.2 embeds as well.

**Reservation.**
- `dlmksym.s` defines `dlm_ksym` with size `KSYM_SPACE` (default 160 KiB), zero-filled, in input section `.ksym`.
- `mac.ld` places `*(.ksym)` at the end of `.data`.
- The size is fixed, so filling the block moves nothing.

**Fill.** `mkksym unix-mac.elf` (host tool, K&R C, byte-offset ELF parsing):
1. Selects every defined `STB_GLOBAL`/`STB_WEAK` symbol, whatever its section, `SHN_ABS` included.
2. Builds the block below.
3. Writes it at the file offset of `dlm_ksym`.
4. Fails if the block is larger than `KSYM_SPACE`.

It runs after the final link and before `elf2coff`. The `ld -r` image is not filled; it never boots.

- **[I]** Current size: 4318 globals, 44 KB of names. The block is ≈ 69 KB of symbols + 44 KB of strings + ≈ 21 KB of hash ≈ 134 KB.
- Local symbols are left out (`-l` includes them for debug builds with a larger `KSYM_SPACE`).

Block layout (big-endian, all offsets from block start):

```c
struct ksymhdr {
	long	kh_magic;	/* 0x4b53594d 'KSYM' */
	long	kh_version;	/* 1 */
	long	kh_nsym;	/* Elf32_Sym entries, [0] is null */
	long	kh_symoff;	/* Elf32_Sym[nsym] */
	long	kh_stroff, kh_strsize;
	long	kh_hashoff;	/* ELF hash: nbucket, nchain, bucket[], chain[] */
	long	kh_lo, kh_hi;	/* kernel image range: start of .text .. end */
	long	kh_size;	/* bytes used */
};
```

- Entries keep `st_value` as absolute run addresses (the image is linked at 0x10000 and runs identity-mapped).
- The hash is the standard ELF hash; `nbucket` is the largest prime ≤ `nsym/4` (≥ 67).
- `dlm_init` accepts the block only if the magic, version and every offset/size fit inside `KSYM_SPACE`, and `kh_lo` = `stext`, `kh_hi` = `end`. Otherwise the DLM stays uninitialised and every DLM call gives `ENOSYS` ([S] §3.1).

**Module tables** ([S] §2 item 19):
- globals only;
- the whole string table;
- an ELF hash with 101 buckets;
- one `kmem` block per module, freed at unload.

**Lookups**:
- by name: hash per image;
- by address: linear scan of the image that contains the address ([S] §3.4).

`PAGESYMTAB` is not implemented; tables are always resident. Kernel-debugger integration and the `/dev/kmem` by-symbol ioctls ([S] §5 item 10) are deferred.

---

## 6. Module file format and loader

### 6.1 File format

One ELF32, `ELFDATA2MSB`, `EM_68K` (4), `ET_REL` object. Accepted sections:

| Type | Treatment |
|---|---|
| `SHT_PROGBITS` + `SHF_ALLOC` (`.text`, `.data`, `.rodata`, …) | copied into the image |
| `SHT_NOBITS` + `SHF_ALLOC` (`.bss`) | zeroed in the image |
| **13** (`SHT_DLMMOD`), `SHF_ALLOC\|SHF_WRITE` | copied into the image; exactly one allowed |
| `SHT_RELA` | applied if its target (`sh_info`) is allocated; skipped otherwise |
| `SHT_REL` | `ERELOC` ([S] §2 item 20: only the native form) |
| `SHT_SYMTAB` (exactly one) + its `SHT_STRTAB` | read, then compacted |
| anything non-allocated (`.comment`, `.shstrtab`, …) | ignored |

**Type-13 section** (named `.moddata` by our tools; the kernel finds it by type, not by name), from [S] Q2:

```
+0  long   address of <prefix>_wrapper      (R_68K_32 against the wrapper symbol)
+4  bytes  dependency names, separated by NUL, blank or tab, to the section end
```

- Size < 4 → `EINVAL` (fix of [S] Q2).
- All names are honoured, not only the first (fix of [S]'s parser defect). Empty tokens are skipped. A name longer than 14 characters → `EINVAL`.
- **Why not [C]'s key=value `.moddata`?** [S] shows the kernel needs only the wrapper pointer and the dependency names. Type, name and keys come from registration, and configuration data lives in the wrapper. [C]'s layout would duplicate registration.
- **Toolchain fact**: binutils 2.8.1 `as` cannot emit a numeric section type (`"aw",13` is rejected). `mkmod` assembles it as `@progbits` with `.balign 4`, links, then rewrites `sh_type` to 13 in the final object.

### 6.2 Relocations emitted by our toolchain

Measured with `m68k-cbm-sysv4-gcc` 2.7.2.3 / `as` 2.8.1 on sample modules (C with `$AMIX_KERNEL_CFLAGS`, with and without `-O2`/`-m68040`; hand-written assembly):

- **C code emits only `R_68K_32`**, in RELA form with the addend in `r_addend` and zero in the field:
  - external calls are `jsr abs.l`;
  - static data is addressed through section symbols (`.bss+0`, `.data+0x50`);
  - switch tables are PC-relative words with no relocation;
  - even calls to globals in the same file are `R_68K_32`.
- `-traditional` puts strings in `.data`; there is no `.rodata`.
- Tentative definitions stay `SHN_COMMON` unless linked with `-d`.
- **Assembly** can also produce:
  - `R_68K_PC32`: `jbsr`/`jbra` to an undefined symbol relax to `bsr.l`/`bra.l`; also `lea sym(%pc)`, `(sym,%pc,Xn)`;
  - `R_68K_PC16` (`bsr.w`, `bra.w`); `R_68K_PC8` (`bsr.s`, addend −1);
  - `R_68K_16` (`sym:w`, `.word sym`), `R_68K_8` (`.byte sym`).
- `as` 2.8.1 refuses PC-relative data relocations (`.long sym-.`).
- **[I]** The kernel image itself contains `R_68K_32` 31036, `PC16` 223, `PC32` 106, `PC8` 1.
- `coff2elf` output (converted A/UX objects) is RELA with `R_68K_32`/`R_68K_PC32` only.

**Loader support**, with S = symbol address, A = `r_addend`, P = run address of the field:

| Type | Value stored (big-endian) | Range check (else `ERELOC`) |
|---|---|---|
| `R_68K_NONE` (0) | nothing | – |
| `R_68K_32` (1) | S + A, 4 bytes | – |
| `R_68K_16` (2) | S + A, 2 bytes | −32768 ≤ v ≤ 65535 |
| `R_68K_8` (3) | S + A, 1 byte | −128 ≤ v ≤ 255 |
| `R_68K_PC32` (4) | S + A − P, 4 bytes | – |
| `R_68K_PC16` (5) | S + A − P, 2 bytes | −32768 ≤ v ≤ 32767 |
| `R_68K_PC8` (6) | S + A − P, 1 byte | −128 ≤ v ≤ 127 |
| 7–22 (GOT, PLT, COPY, GLOB_DAT, JMP_SLOT, RELATIVE) and unknown | – | always `ERELOC` |

- The field is overwritten, not added to (RELA). Stores are byte-wise, so odd offsets (`PC8` at `bsr.s`+1) and misaligned fields are fine.
- `r_offset` must satisfy `r_offset + size ≤ sh_size` of the target.
- `ELF32_R_SYM` must index the symtab named by `sh_link`, which must be the module's only symtab.
- Symbol index 0 means S = 0.
- `STT_SECTION` symbols resolve to that section's load address. A relocation against a non-allocated section's symbol → `ERELOC`.

### 6.3 Symbol resolution ([S] §2 item 15; [S] wins over [C])

For each symbol:

| `st_shndx` | Value |
|---|---|
| defined in an allocated section | section load address + `st_value` |
| `SHN_ABS` | `st_value` |
| index of a non-allocated section, or out of range | `ERELOC` |
| `SHN_UNDEF` | search in order: (1) the module's own globals; (2) its **direct** dependencies, the last listed first; (3) the static kernel. Not found: weak → 0, else logged and `ERELOC` after the whole table is scanned (so every missing name is logged) |
| `SHN_COMMON` | search (2) then (3) first; if found, bind to it; else allocate `st_size` bytes aligned to `st_value` in the image's common area |

- Transitive dependencies are not searched.
- A dependency that is statically configured adds no search entry; its symbols are in the kernel table.
- "Found" is a flag, not a non-zero value (fixes [S]'s value-0 quirk).
- A weak unresolved reference is not entered in the module's table.
- Among a module's own definitions, a strong one wins over a weak one. The same name defined strong twice → `ERELOC`.

### 6.4 Load sequence

Every path (demand, device open, push, mount, exec, syscall, dependency, stub) runs `dlm_load(name_or_path, cred, root, &mod)`.

1. **Checks.** Uninitialised → `ENOSYS`. Name in the sorted static-module list (`dlm_static[]`, generated at relink from the static objects' wrapper symbols plus a manual list) → `EINVAL`.
2. **Record.** Make a record marked *loading*, owned by `u.u_procp`. Search the list by name:
   - owned by us → cycle, `EINVAL`;
   - in transition → sleep on the record at `PZERO`, then search again;
   - settled → free ours and return the existing one.
3. **Id.** Append the record and assign `dlm_nextid++` (ids start at 1, never reused, consumed even on failure; [S] Q9).
4. **Credentials.** Arm `DLM_GUARD`. Auto-loads swap `u.u_cred` for `dlm_syscred` (built in `dlm_init` with `crget`, all ids 0) and `u.u_rdir` for NULL ([S] §2 item 27). Both are restored on every exit.
5. **Open.** `vn_open` each candidate path (`FREAD`), then `vn_rdwr` with the same credential. Read the ELF header and all section headers into one `kmem` buffer (never on the 8 KB kernel stack).
6. **Validate** the ELF header and sections (§6.1). Read the type-13 section's bytes, and from them the dependency names.
7. **Dependencies.** Load each with system credentials and root, not demand-marked, raising its `deps` (unlinking it from the candidate list). Recursion depth ≤ 8. Frames stay small; the names are copied to the heap first.
8. **Symbols.** Read symtab and strtab; resolve (§6.3). Commons are sized here.
9. **Image.** One block = allocated sections (`sh_addralign` each, minimum 4) + commons (own alignment each), from `kmem_zalloc(size + 15)`, then aligned to 16. Pointer and size are kept for `kmem_free`. Over `dlm_maximage` → `ENOMEM`. Read `PROGBITS` and type-13 contents into place.
10. **Relocate** (§6.2).
11. **Cache flush** (§8.3).
12. **Tables.** Build the module's global table, mark it *usable* (from now on `getksym` sees it), close the file, restore credentials.
13. **Revision.** Wrapper = word 0 of the type-13 section. `mw_rev` ≠ `MODREV` → `EBADVER`.
14. **Delay.** From `mw_conf->mcd_unload_delay` (seconds; wrapper data absent → `dlm_def_unload_delay`), converted to ticks (`HZ` 60).
15. **Load routine.** Call `mw_load()` if non-NULL. An error undoes everything and is returned.
16. **Install** every linkage (§8.2). On failure: remove the linkages already installed, call `mw_unload`, undo everything, and return the error.
17. **Stubs.** Patch stubs (§11). On failure: full unload; panic if that fails; `ERELOC`.
18. **Settle.** Clear *loading*, wake waiters, return.

- **Error path** ([S] §3.3): free the image, symbols and buffers; release each dependency's `deps` (each may become a candidate); unlink and free the record; wake waiters; the id stays consumed.
- The step order differs from SVR4.2 (sizing needs the symbols before the single allocation), which only changes which error is reported when a file has several faults.
- **Callers afterwards**: `modload` sets the demand mark; open/push/mount/exec/syscall trampolines hold; stubs take a permanent hold; the dependency path raises `deps`.

---

## 7. Unload

### 7.1 Candidate list

([S] §2 item 23, with its fixes)
- A module joins the list, stamped with `lbolt`, when `refs` falls to 0 with `deps` 0, or when `deps` falls to 0 with `refs` 0.
- It leaves when either count rises from 0.
- A freshly loaded unused module is not on the list.
- Demand-marked modules may be on it; the automatic path refuses them.

### 7.2 Busy test

Busy if `refs`, `deps`, `incall` or profiler lock is set, or the module is loading or unloading.

### 7.3 Unload sequence

([S] §2 item 22). The caller marks the module *unloading*; any failure clears the mark.

1. **Refuse.** Automatic path and demand-marked → refuse. Busy → `EBUSY`.
2. **Remove linkages.** Each slot goes back to its placeholder, so auto-load stays armed. Hooks go back to their defaults.
3. **Unload routine.** Call `mw_unload(modp)` if non-NULL (the argument is opaque).
   - NULL means nothing to undo, for **every** wrapper type ([S] §2 item 14; [C]'s "NULL = never unload" dropped).
   - A module that must stay resident returns `EBUSY` (HBA convention).
   - On error: reinstall the linkages (panic if that fails), clear *unloading*, return the error.
4. **Reset stubs.** Reset this module's stub descriptors to their initial targets (fix of [S] Q4).
5. **Free.** Free the image and the symbol block. Release the dependencies' `deps` (each may become a candidate, stamped now).
6. **Remove.** Unlink from both lists, wake waiters, free the record.

### 7.4 Auto-unload daemon

[S]: SVR4.2 auto-unloads from the swapper under memory pressure, with no periodic daemon. [C]/UnixWare: a periodic daemon with `UNLOAD_WAKE`. AMIX `sched` is an endless loop in binary code and cannot be wrapped, so both behaviours run in one kernel-context daemon:

- `/etc/conf/bin/moddaemon`, started from `/etc/inittab` (`mdl:23:respawn:`), calls `modadm(MOD_TY_NONE, MOD_C_AUTOUNLD, 0)`. The call needs privilege, never returns (only on a signal, with `EINTR`), and allows one daemon (a second gets `EBUSY`).
- **Loop**: sleep at `PZERO+1` (interruptible) for 1 s (`timeout`/`wakeup`), then:
  - **Pressure pass** ([S] Q6): if `freemem < desfree`, walk the candidate list oldest first. Skip modules in transition or with `lbolt − stamp < delay`. Try each (§7.3, automatic). After each success stop once `freemem ≥ lotsfree`.
  - **Periodic pass**: if `dlm_unload_wake` > 0 and that many seconds have passed since the last one, walk the whole candidate list the same way, without the memory stop.
- **Tunables** (in `dlmconf.c`): `dlm_def_unload_delay` 60 s (`DEF_UNLOAD_DELAY`); `dlm_unload_wake` 60 s (`UNLOAD_WAKE`; 0 = pure SVR4.2 behaviour); per-module `PREFIX_UNLOAD_DELAY` compiled into `<prefix>_conf_data` by `mkmod`.
- Without the daemon running, nothing auto-unloads. Demand unload still works.

### 7.5 Profiler lock

Ours; [S] Q7 shows SVR4.2's lock was buggy.
- W-wrap `prfopen`: on the first open, lock every loaded module and set the kernel-wide mark.
- While the mark is set, newly loaded modules are locked too (the safer choice in [S]).
- W-wrap `prfclose` (last close): clear every mark. Clearing is idempotent.
- A locked module counts as busy.

### 7.6 Halt

The `dhalt` wrapper (§4.2) calls `mw_halt` of every loaded module that has one, in load order ([S] §2 item 25).

---

## 8. Kernel internals

### 8.1 `dlm_init` (from `dmainit`)

1. Validate the ksym block (§5); on failure print one line and return with the DLM disabled.
2. Copy `__amix_fmodsw`, `__amix_vfssw`, `__amix_execsw` rows into the enlarged tables. Record the capacities.
3. Write `sysent[64..69]`. Assert first that `sysentsize` = 142 and that every slot the DLM claims still holds `nosys`; else disable the DLM.
4. Build `dlm_syscred`. Set the default path to `/etc/conf/mod.d`.
5. Mark the DLM initialised.

### 8.2 Wrappers and linkages (`<sys/moddefs.h>`)

```c
#define MODREV	1

struct mod_operations {			/* one per linkage type, in the kernel */
	int	(*modm_install)();	/* (struct dlm_mod *, void *typedata) */
	int	(*modm_remove)();
	void	(*modm_info)();		/* fills a struct modspecific_stat */
};
struct modlink { struct mod_operations *ml_ops; void *ml_type_data; };
struct mod_type_data { char *mtd_desc; void *mtd_pdata; };
struct mod_conf_data { int mcd_unload_delay; };	/* seconds */

struct modwrapper {
	int	mw_rev;
	int	(*mw_load)();
	int	(*mw_unload)();
	void	(*mw_halt)();
	struct mod_conf_data *mw_conf;
	struct modlink *mw_modlink;	/* ends with { 0, 0 } */
};
```

Kernel linkage operations: `mod_drvops`, `mod_strops`, `mod_fsops`, `mod_miscops`, `mod_execops`, `mod_sysops`, `mod_hookops`.

Per-type private data (`mtd_pdata`), generated by `mkmod` into `<name>_conf.c`:

```c
struct mod_drv_data  { struct bdevsw drv_bdevsw; int drv_bmajor, drv_bcount;
		       struct cdevsw drv_cdevsw; int drv_cmajor, drv_ccount; };
struct mod_str_data  { char str_name[FMNAMESZ+1]; struct streamtab *str_tab; int *str_flag; };
struct mod_fs_data   { char *fs_name; int (*fs_init)(); struct vfsops *fs_ops; long fs_flag; };
struct mod_exec_data { short ex_magic; short ex_flags; int (*ex_func)(); int (*ex_core)(); };
		       /* array, ends with ex_func == 0 */
struct mod_sys_data  { int sy_num; char sy_narg, sy_flags; int (*sy_call)(); };
		       /* array, ends with sy_call == 0 */
struct mod_hook_data { char *hk_name; void (*hk_fn)(); };	/* ends with hk_name == 0 */
```

Wrapper macros. Each defines the global `<prefix>_wrapper` and one linkage. Generated data is referenced weakly (top-level `asm(".weak ...")`; under `-traditional`, pasting uses `prefix/**/_wrapper` and the name is substituted inside the string literal; `__STDC__` builds use `##` and `#`), so a module linked statically without generated data still links ([S] Q3):

| Macro | Args | Linkage (type data) |
|---|---|---|
| `MOD_DRV_WRAPPER` | prefix, load, unload, halt, desc | `mod_drvops` (`<prefix>_drvdata`) |
| `MOD_HDRV_WRAPPER` | prefix, load, unload, halt, desc | `mod_miscops` (HBA: no slot, no auto-load; `_unload` returns `EBUSY` by convention) |
| `MOD_STR_WRAPPER` | prefix, load, unload, desc | `mod_strops` (`<prefix>_strdata`) |
| `MOD_FS_WRAPPER` | prefix, load, unload, desc | `mod_fsops` (`<prefix>_fsdata`) |
| `MOD_MISC_WRAPPER` | prefix, load, unload, desc | `mod_miscops` |
| `MOD_EXEC_WRAPPER` (ours) | prefix, load, unload, desc | `mod_execops` (`<prefix>_execdata`) |
| `MOD_SYSCALL_WRAPPER` (ours) | prefix, load, unload, desc | `mod_sysops` (`<prefix>_sysdata`) |
| `MOD_HOOK_WRAPPER` (ours) | prefix, load, unload, desc | `mod_miscops` + `mod_hookops` (`<prefix>_hookdata`) |

All take `<prefix>_conf_data` weakly. `MOD_ACDRV_*` (UnixWare 7) is not provided.

Install rules per linkage:

- **drv**
  - Every major in `[bmajor, bmajor+bcount)` and `[cmajor, cmajor+ccount)` must be < the table size (else `ECONFIG`) and hold this module's placeholder of the right kind (else `EINVAL`).
  - Flags must not include `D_OLD` (`EINVAL`).
  - Copy the image, keeping the trampolines and the name.
  - `modm_info` reports block and char pairs ([S] reports "block driver" always; we report the true type: `MOD_TY_CDEV`, `MOD_TY_BDEV` or `MOD_TY_SDEV`).
- **str**: the name's `fmodsw` slot must be this module's placeholder. Build the pool wrapper; set `f_str`/`f_flag`. Info: slot index, −1.
- **fs**
  - The name's `vfssw` slot must be a placeholder without owner (a second install → `EINVAL`, not [S]'s panic).
  - Call `fs_init(&vfssw[i], i)` if non-NULL. This is our addition: loadable fs code often needs its type index.
  - Save `fs_ops`; set `vsw_flag`. Info: slot index.
- **exec**, **sys**, **hook**: §9.
- **misc**: nothing.

Module API (DDI additions, in `<sys/moddefs.h>`):
- `int mod_hold(struct modwrapper *)` / `void mod_rele(struct modwrapper *)` map a wrapper to its module; for a static wrapper they are no-ops returning 0.
- `mod_drvattach(struct mod_drvattach *)` / `mod_drvdetach(...)` (§8.4).

### 8.3 Cache maintenance after loading text

Runs after relocation, before anything can execute the image, at `splhi`, in `dlm_cache.s`:

- `cputype` ≥ 60 (68060): `cpusha bc`, then `movec cacr,d0; or.l #0x00400000,d0` (CABC, clear branch cache) `; movec d0,cacr`.
- `cputype` ≥ 40 (68040): `cpusha bc`.
- otherwise (68020/030): `movec cacr,d0; or.w #0x0808,d0` (clear data and instruction caches) `; movec d0,cacr`.

Rules:
- Read `cputype` as a long (the port's definition, see its `fpe040.s` note).
- Whole-cache, not ranged, as the port does for code publication (`codepub040.s`, `cb_icode040.s`). `cpushl` needs physical addresses, and loads are rare.
- `cinva` alone would lose modified lines, and `cinva dc` would discard them.
- No flush is needed on unload or for table edits (data only). Stub descriptors are data.

### 8.4 Memory

- Images, symbol blocks, side-table entries, open-set entries and loader buffers come from `kmem_alloc`/`kmem_zalloc(size, KM_SLEEP)`. They are freed with `kmem_free(p, size)` using the recorded size.
- The kernel's heap is supervisor-only and mapped in every address space (the kernel window lives in the SRP tree), which meets [C] R6.
- Interrupt attach: `mod_drvattach` walks `{int source; int ipl; void (*handler)(); int arg;}` entries (list ends with `source < 0`) and calls the platform's `plat_intr_attach(source, ipl, handler, arg)` / `plat_intr_detach(...)`, defined by the platform layer (Mac: VIA1/VIA2/slot sources; Amiga: the `int2_tbl` chain).
  - Unlike [S], conflicting priorities and detaching a handler that isn't the caller's are rejected (`EINVAL`), per [S] §5 item 13.
  - Until a platform provides these, `mod_drvattach` returns `ENXIO`, and only interrupt-free modules load.

### 8.5 Serialisation

- The kernel is uniprocessor and non-preemptive. There is no global lock, as in SVR4.2 ([S] §2 item 18).
- Per-module *loading*/*unloading* marks and the owning process give the exclusion; waiters sleep on the record.
- Registration and slot edits do not sleep between check and update.
- `getksym` and dependency searches mark a module `incall` while reading its table, instead of holding it.

### 8.6 DLM syscall guard

Each DLM system call also arms `DLM_GUARD`. A `longjmp` inside `dlm_load` runs the §6.4 error path and returns `EINTR`. Inside `dlm_unload` it clears *unloading* and returns `EINTR`.

---

## 9. Extensions beyond SVR4.2

SVR4.2 has neither loadable exec formats nor loadable syscalls ([S] Q3, Q5, §5 item 16). Both are new designs on its counting model: any positive count blocks unload.

### 9.1 Exec-format modules

- **Registration** (`MOD_TY_EXEC`): one per magic.
  - `er_flags & EXF_FIRST`: insert before every existing `execsw` row with the same magic, shifting later rows down by one.
  - Otherwise append.
  - Shifting is safe: `gexec` only reads the table until it calls a function, and it stops after that call (**[I]**, `aux-kernel-design.md` §2.2). `execsw` pointers are not kept anywhere else: `exec_core` has no text references.
  - `nexectype` grows by one.
- **Entry**: `exec_magic` → the slot's short; `exec_func` = `dlm_exec_K` (one small C function per slot K, bound to the registration, not to the row, so rows can move). It takes eight long arguments and passes them on unchanged; that covers every AMIX exec function (≤ 6 arguments).
- **`dlm_exec_K`**, under `DLM_GUARD`:
  1. Auto-load if needed.
  2. `hold`.
  3. Call the real `ex_func`.
  4. `release`.
  5. If the load failed, or the function returned `ENOEXEC`, **and** the slot has `EXF_FIRST`: continue with the next row with the same magic, and return its result. Otherwise return the result (load failure → `ENOEXEC`).

  So a claim-first format module can decline a file, and a missing module never blocks the static loader behind it.
- **Lifetime** (answers [C] Q5): the trampoline's hold covers the exec call only. A module whose code serves the process afterwards (signal frames, syscall translation) takes `mod_hold(&<prefix>_wrapper)` per process and `mod_rele` when the process leaves the format (next exec or exit). The A/UX personality does this in its `aux_proc` create/free.
- **Install**: every `ex_magic` in `<prefix>_execdata` must have a registration by this module (else `EINVAL`). `exec_core` is saved but unused (AMIX calls none).

**A/UX use.** Register magic 0x150 with `EXF_FIRST`. `aux_coffexec` returns `ENOEXEC` for SVR3 COFF (`STYP_LIB` present), and the trampoline then falls through to stock `coffexec`. This replaces the static retarget of `execsw[0]` in `aux-kernel-design.md` §9.2 step 4 when the personality is built as a module. Before the module is loaded, the first COFF exec auto-loads it once.

### 9.2 Syscall modules

- **Slots**: `sysent` 70–77, 82, 83, 105, 140 (12). Numbers are fixed per module in its Master file (`$syscall num narg [setjmp]`) so user libraries can hard-code them.
- **Registration** (`MOD_TY_SYS`):
  - the number must be one of the 12 slots and hold `nosys` or this module's trampoline (else `EEXIST`);
  - `narg` ≤ 8;
  - it writes `sy_narg`, `sy_flags` (`SETJUMP` if asked) and `sy_call` = `dlm_sys_K`. The count is fixed before load because `systrap` copies arguments before calling.
- **`dlm_sys_K(uap, rvp)`**, under `DLM_GUARD`: auto-load (failure → `ENOSYS`), `hold`, call, `release`. The candidate stamp therefore measures time since the last call.
- **Install**: each `sy_num` must be registered to this module, and `sy_narg`/`sy_flags` must match (else `EINVAL`).

### 9.3 Hook modules (A/UX syscall gates and other static hooks)

- The personality's entry points (vector gates for trap #0/#15, A-line, privilege, trace, fault reflection, `ev_*` stubs, `sendsig`/`valid_usr_range`/`fsig`/`usrxmemflt` wrappers; `aux-kernel-design.md` §1, §3) stay **static**, because they are overrides and vector retargets.
- Each static shim calls through a named hook pointer. It falls back to stock behaviour when the pointer is NULL.
- `hooksw[]` is a static table `{char *name; void **ptr; void *dflt; struct dlm_mod *owner;}` generated at relink from the shim objects.
- **`mod_hookops` install**: for each `<prefix>_hookdata` entry the name must exist and be unowned (else `EEXIST`); set `*ptr` and the owner. Remove restores `dflt`.
- **Safety rule for shims**: call a hook only for a process that holds the module (A/UX: `p_evpdp` ≠ NULL implies an `aux_proc`, which implies `mod_hold`), or else bracket the call with `dlm_enter(owner)`/`dlm_exit(owner)` (`incall`). Native processes test `p_evpdp` and never enter module code.
- **Layout for A/UX**:
  - `auxcore`: `MOD_HOOK_WRAPPER` (translator, signals, CPU virtualization);
  - `auxexec`: `MOD_EXEC_WRAPPER`, `$depend auxcore`;
  - `uinter`: `MOD_DRV_WRAPPER`, `$depend auxcore`.

---

## 10. Headers and user-visible structures

`kernel/dlm/include/sys/` (an overlay placed before the AMIX include directory):

- **`errno.h` additions**: §2.2.
- **`mod.h`**: prototypes of the six calls; `MOD_TY_*`, `MOD_C_MREG` 1, `MOD_C_AUTOUNLD` 100, `EXF_FIRST` 1; `struct mod_mreg`, `struct mod_execreg`, `struct mod_sysreg`; `MODMAXNAMELEN` 15, `MODMAXLINK` 4, `MODMAXLINKINFOLEN` 32. The status structure:

```c
struct modspecific_stat {
	char	mss_linkinfo[MODMAXLINKINFOLEN];	/* wrapper description */
	int	mss_type;				/* MOD_TY_* */
	int	mss_p0[2];				/* see table */
	int	mss_p1[2];
};
struct modstatus {
	int	ms_id;
	caddr_t	ms_base;		/* image */
	u_int	ms_size;		/* image incl. commons */
	int	ms_rev;
	char	ms_path[MAXPATHLEN];
	time_t	ms_unload_delay;	/* seconds */
	int	ms_refcnt;
	int	ms_depcnt;
	struct modspecific_stat ms_msinfo[MODMAXLINK];
	char	ms_name[MODMAXNAMELEN];	/* ours; SVR4.2 derives it from the path */
	int	ms_flags;		/* MS_DEMAND 1, MS_LOCKED 2, MS_CAND 4 (ours) */
};
```

| Linkage | `mss_p0` | `mss_p1` |
|---|---|---|
| drv | first bmajor, bcount | first cmajor, ccount |
| str | fmodsw index, −1 | – |
| fs | vfssw index, – | – |
| exec | magic, execsw row | – |
| sys | syscall number, narg | – |
| misc, hook | – | – |

- **`ksym.h`**: `MAXSYMNMLEN` 256, `getksym` prototype, `struct ksymhdr` (§5).
- **`moddefs.h`**: §8.2.

---

## 11. Loadable stubs (optional, last stage)

These follow [S] Q4, with its fix.

- For a base-system module flagged `l`, relink adds to the static kernel:
  - `<name>_modinfo = {name, descriptors}`;
  - one stub per exported function, with **the real function's name**, jumping indirectly through a descriptor `{target, info, self, errfn}`.
- **Normal stub**: the trampoline calls `dlm_stubload(desc)`.
  - This auto-loads with system credentials and takes a **permanent** hold (the module never unloads).
  - On success it jumps to the patched target with the original arguments.
  - On failure it jumps to `errfn` (return 0, −1, `EINVAL` or `ENOLOAD`), or panics if there is none.
- **Weak stub**: the target is `errfn` from the start.
- **Patching**: after every successful load, look up `<name>_modinfo` (kernel table, then modules). For each descriptor, reverse-map `self` to its name and resolve that name in the new module. Failure → `ERELOC`, with full unload.
- **Unload** resets every descriptor of the module (fix of [S]'s dangling-target defect).

---

## 12. User-space tools and build side

### 12.1 On the target (K&R C, `libmod.a` stubs)

**`modadmin`** ([C] §1.2), using §2:

| Form | Action |
|---|---|
| `-l name\|path…` | `modload`; print the ids |
| `-u id…` | `moduload`; 0 = all |
| `-U name…` | find the id by `ms_name` (fall back to the path's last component, as SVR4.2), then `moduload` |
| `-q id…`, `-Q name…` | full status (all `modstatus` fields, types by name, delay in seconds) |
| `-s`, `-S` | iterate `modstat(1, …, 1)`, then `modstat(id+1, …, 1)` until `EINVAL` |
| `-d dir[:dir]` | `modpath` |
| `-D` | `modpath(NULL)` |

It prints its own messages for errno 164–168. Exit status 1 if any operand failed.

**`idmodreg [-f file]`** replays `/etc/mod_register` (default). One registration per line, `#` comments:

```
cdev   <modname> <major>
bdev   <modname> <major>
sdev   <modname> <major>
str    <modname> [<pushname>]
fs     <modname> <fstype>
exec   <modname> <magic> [first]
sys    <modname> <number> <narg> [setjmp]
misc   <modname>
```

It continues past errors, reports each, and exits 1 if any failed.

**`idmodload [-r root] [-f list]`** demand-loads each name or absolute path in `/etc/loadmods` (one per line, `#` comments). It continues past failures and exits 1 if any failed ([C] §1.2).

**`moddaemon`** (§7.4).

**`/etc/inittab`** entries, in order:

```
mdr::sysinit:/etc/conf/bin/idmodreg >/dev/sysmsg 2>&1
mdl::sysinit:/etc/conf/bin/idmodload >/dev/sysmsg 2>&1
mdd:23:respawn:/etc/conf/bin/moddaemon
```

**Directory layout**: `/etc/conf/mod.d/<module>` (loadable files); `/etc/conf/bin/`; `/etc/mod_register`; `/etc/loadmods`.

### 12.2 Build side (cross, in `tools/dlm/`, K&R C, self-hosting)

**`mkksym`** (§5): fill the block; `-c` re-reads the ELF and checks that every global resolves through the hash to its `nm` value.

**`dlmslots <image>`**: from `.rela.data` of the `ld -r` image, list the empty `cdevsw`/`bdevsw` rows (§3.2) → `build/dlm.majors`.

**`mkmod`**, the `idbuild -M` equivalent. Per module directory:
- **Inputs**:
  - `Driver.o`;
  - optional `Space.c` and `Stubs.c` (the latter is ignored for loadable builds);
  - `Master`, restricted to the final line `name prefix flags order bmaj cmaj` with flags `b c S m F e s h L l k u o`, and `$depend`, `$modtype`, `$magic n… [first]`, `$name`, `$syscall num narg [setjmp]`;
  - optional `System` (`name Y|N …`; extra PC fields ignored);
  - optional `Mtune` (`PREFIX_UNLOAD_DELAY`).
- **Steps**:
  1. Assign majors: `k` = as given; otherwise the lowest unused from `dlm.majors`.
  2. Find the entry points present by scanning `Driver.o`'s symtab for `<prefix>open`, `close`, `read`, `write`, `ioctl`, `mmap`, `segmap`, `poll`, `strategy`, `print`, `size`, `halt`, and `<prefix>info` (streamtab).
  3. Generate `<name>_conf.c`: `<prefix>_conf_data`, and `<prefix>_drvdata`/`strdata`/`fsdata`/`execdata`/`sysdata` with `nodev` for absent entries and the flag word (`D_OLD` rejected).
  4. Compile it and `Space.c` with `$AMIX_KERNEL_CFLAGS` (`-m68020` baseline, no FPU code).
  5. Generate and assemble `moddep.s` (§6.1).
  6. `m68k-cbm-sysv4-ld -r` (no `-d`, so commons reach the loader) → `mod.d/<name>`.
  7. Rewrite the `.moddata` type to 13.
- **Checks (fail closed)**:
  - exactly one `<prefix>_wrapper`;
  - one type-13 section ≥ 4 bytes whose word 0 is relocated against the wrapper;
  - every relocation type in §6.2's supported set;
  - every undefined symbol found in the kernel's exported list (`mkksym -x`) or in a `$depend` module's exports;
  - name ≤ 14 characters.
- **Outputs**: the registration lines → `mod_register.d/<name>`; the `/etc/loadmods` line if the System file asks for load at boot.

**Relink stage** (`relink-mac.sh`; the same for other platforms):
- DLM objects: `dlm_core.c dlm_ld.c dlm_sym.c dlm_reg.c dlm_sw.c dlm_str.c dlm_fs.c dlm_exec.c dlm_sys.c dlm_hook.c dlm_unld.c dlm_tramp.c dlm_cache.s dlmksym.s dlmconf.c`.
- **Overrides**:
  - `OVR += dmainit`;
  - **W** (weaken + `__amix_X` alias at the stock address): `dounmount dhalt prfopen prfclose`;
  - data `OVRD += fmodsw vfssw execsw`, each with an `__amix_` alias.
- Each must bind exactly once in our objects, as the existing override check does.
- Then: final link, `mkksym`, `mkksym -c`, `elf2coff`.

---

## 13. Resolved differences between [C] and [S]

| # | Topic | [C] | [S] | Decision |
|---|---|---|---|---|
| 1 | bad ELF/machine/type | `ERELOC` | `EINVAL` | [S] |
| 2 | header read errors | – | passed through | [S] |
| 3 | out of memory | `ENOMEM` | never (sleeps) | sleep; `ENOMEM` only above `dlm_maximage` (ours) |
| 4 | undefined symbol report | console | message buffer | [S], plus `dlm_verbose` |
| 5 | modload of a loaded module | return id | return id, set demand mark | [S] |
| 6 | moduload failure "candidate" | queued | demand mark cleared | [S] |
| 7 | moduload(0) | repeat passes | restart after each success; `EBUSY` if none; `EINVAL` if none loaded | [S] |
| 8 | modstat privilege | yes | yes | same |
| 9 | modstat on a module in transition | – | `EINVAL`, ends iteration | skip when iterating (fix) |
| 10 | modstat bad buffer | `EFAULT` | raw value | `EFAULT` (fix) |
| 11 | modstat name field | yes | none | kept as an appended field |
| 12 | getksym direction | name first, address on failure | by `*value` == 0, name written back | [S] |
| 13 | registration commands | REG, UNREG, REGLIST | register only | [S] |
| 14 | registration type check | – | off by one | fixed |
| 15 | STREAMS registration name | – | file name only | honour the datum (allowed by [S]) |
| 16 | metadata in module | `.moddata` key=value | type-13 section: wrapper word + deps | [S] layout, all deps honoured (fix) |
| 17 | wrapper found by | symbol `*_wrapper` | relocated word | [S] |
| 18 | revision check | before load | after relocation | [S] |
| 19 | wrappers | + EXEC, SYSCALL, HOOK, ACDRV | exactly five | five + EXEC, SYSCALL, HOOK; no ACDRV |
| 20 | NULL `_unload` | never unload drivers | nothing to undo | [S] |
| 21 | resolution order | own, kernel, deps (transitive) | own, direct deps newest first, kernel | [S]; found flag replaces value ≠ 0 |
| 22 | commons | own block | kernel/deps first | [S] rule, one allocation |
| 23 | kernel symtab | embed or load | embedded | embedded in a `.data` reservation |
| 24 | `PAGESYMTAB` | dropped | exists | dropped |
| 25 | auto-unload | periodic + low water | swapper, pressure only | both, in a daemon (§7.4) |
| 26 | candidate list | – | two defects | fixed |
| 27 | profiler | lock while on | buggy inversion | lock while `/dev/prf` is open |
| 28 | stubs | unclear | auto-load, permanent hold | [S] + unload reset |
| 29 | static wrapper modules | `_load` from init | wrappers ignored, name list | [S] |
| 30 | reference mechanism | trampolines | core owner tables | slot trampolines + open sets + side table; `dounmount` wrap (§4.2) |
| 31 | dependency errors | – | `EINVAL` | [S] |
| 32 | mount auto-load failure | load's error | `ENOLOAD` | [S] |
| 33 | tunables | `*_RESERVE`, `UNLOAD_WAKE`, `MOD_MAXLOADED` | `*_RESERVE`, delays, `PAGESYMTAB` | device reserves replaced by empty-row pool; `FMOD/VFS/EXEC_RESERVE` compile-time; `UNLOAD_WAKE` kept; `MOD_MAXLOADED` dropped |
| 34 | `D_OLD` loadables | – | per-slot compat | not supported |
| 35 | credentials for auto-load | root | system cred + system root | [S] |
| 36 | exec/syscall lifetime | per call + `mod_hold` | none in SVR4.2 | per call + `mod_hold` |

---

## 14. Staged implementation plan

Every stage ends with **static** checks only: builds, image inspection, and host-compiled test harnesses (the kernel C files compiled with the host `cc` against stubbed kernel services, as `kernel/mac/integrate/verify.sh` does for root selection). Runtime checks for later boots are listed but are not pass criteria.

**Stage 0: headers, symbol table, tools skeleton.**
- **Deliver**: §10 headers; `libmod.a`; `mkksym`; `dlmslots`; the `.ksym` reservation and `mac.ld` line; `dlm_init` limited to the ksym check.
- **Pass**:
  - `sh kernel/build.sh` passes all stages;
  - `mkksym -c` resolves all globals (count = the `nm` global count);
  - the block fits `KSYM_SPACE`;
  - `dmainit` bound once in our object;
  - 0 unresolved, validator 0 complaints;
  - `elf2coff -c` ACCEPTED.

**Stage 1: loader, MISC modules, syscalls, demand load/unload.**
- **Deliver**: §2, §5 module tables, §6, §7.1–7.3, §8.1–8.3, §8.5–8.6; `modadmin`; `mkmod` for MISC.
- **Pass**:
  - **Relocation oracle**: host-built `dlm_ld.c` loads each test module at base B against the table from `unix-mac.elf`; the image must be byte-identical to `m68k-elf-ld -r`-then-`-Ttext=B --just-symbols=unix-mac.elf` output. Modules: the C samples, an asm module with all seven relocation types, a `coff2elf`-converted A/UX object.
  - **Negative files** give the §2.3 table's errnos: bad magic, `EM_386`, `ET_EXEC`, `SHT_REL`, GOT reloc, `PC16` overflow, missing type 13, 2-byte type 13, undefined symbol, `mw_rev` 2, a two-module cycle, depth 9.
  - **Resolution harness**: dependency shadowing (the dependency wins over the kernel), transitive symbol rejected, weak undefined = 0, common bound to a kernel global versus allocated.
  - **State harness**: id sequence with gaps; `modstat` iteration skips a *loading* record; `moduload(0)` over a 3-chain; candidate list invariants over random hold/rele/dep sequences (never double-linked).
  - **Image**: `sysent` claims asserted in `dlm_init`; 0 unresolved; validator 0.
  - `mkmod` builds `dlmtest` (MISC, `$depend` on a second MISC) with every undefined symbol in the kernel list.

**Stage 2: STREAMS modules.**
- **Deliver**: `fmodsw` override and copy; `MOD_TY_STR` registration; STREAMS pool; `dlm_str_open`/`close`; side table; `mod_strops`; `idmodreg`/`idmodload`.
- **Pass**:
  - image: `fmodsw`, `fmodcnt` users (`findmod`, `qattach`, `strioctl`, `getmid`, `getadmin`, `apush_iocdata`) all bind to the new `fmodsw` (relocation scan); `__amix_fmodsw` equals the stock address;
  - harness drives push, reopen, pop, close, failed open and a `longjmp` during open, and checks refs and side-table size return to 0;
  - a sample `nullmod` builds and passes `mkmod` checks.

**Stage 3: drivers (char, block, STREAMS, clone) and interrupts.**
- **Deliver**: `MOD_TY_CDEV/BDEV/SDEV`; device trampolines and open sets; `mod_drvops`; `mod_drvattach` over `plat_intr_*` (Mac first); `mkmod` driver data.
- **Pass**:
  - harness covers opens and closes per `otyp`, `OTYP_LYR` pairs, self-cloning `*devp` changes, clone via `clnopen`'s sequence, and wrong-majors `ENXIO`;
  - `dlmslots` output matches the Mac image's empty rows;
  - the §4.2 verification of `d_close` per `otyp` is done by disassembly and recorded, and the key is fixed accordingly;
  - a sample driver (a loadable `/dev/null` clone) builds.

**Stage 4: file systems.**
- **Deliver**: `vfssw` override and copy; `MOD_TY_FS`; vfsops trampolines; `dounmount` W-wrap; `fs_init`.
- **Pass**:
  - all 29 `vfssw` and 18 `nfstype` references bind correctly, including the port's `sync`;
  - harness: mount, remount, failed mount, unmount, auto-load failure → `ENOLOAD`, and `sync` through an unloaded or loaded slot leaving `refs` unchanged (`incall` only).

**Stage 5: auto-unload, halt, profiler.**
- **Deliver**: `MOD_C_AUTOUNLD` and `moddaemon`; pressure and periodic passes; `dhalt`, `prfopen`, `prfclose` wraps.
- **Pass**:
  - harness with a simulated `freemem`/`lbolt`: pressure stop at `lotsfree`, delay respected, demand-marked refused, periodic pass with `dlm_unload_wake` = 0 disabled;
  - profiler lock is idempotent, and newly loaded modules are locked while it is held;
  - W-wraps bound once each.

**Stage 6: exec modules.**
- **Deliver**: `execsw` override; `MOD_TY_EXEC` with `EXF_FIRST`; `dlm_exec_K`; `mod_execops`.
- **Pass**:
  - `gexec`'s references bind to the new `execsw`;
  - harness: first-claim insert shifts rows; `ENOEXEC` and load-failure fall-through reach stock `coffexec`; per-process `mod_hold` blocks unload;
  - `auxexec` skeleton builds as a module.

**Stage 7: syscall and hook modules; A/UX packaging.**
- **Deliver**: `MOD_TY_SYS` and `dlm_sys_K`; `hooksw` generation and `mod_hookops`; `MOD_HOOK_WRAPPER`; `auxcore`/`auxexec`/`uinter` split per §9.3.
- **Pass**:
  - harness: registration rejects non-reserved numbers and `narg` > 8; the syscall guard balances refs on `longjmp`; hook install, remove and conflict;
  - the static shim image tests `p_evpdp` before any hook call (disassembly check of each gate).

**Stage 8 (optional): loadable stubs** (§11).
- **Pass**: harness for patch, reset on unload, weak stub, and `errfn` paths.

**Runtime checks for the first boots** (QEMU `q800` or hardware; not pass criteria here):
- `modadmin -l dlmtest`, `-S`, `-u`;
- push and pop of `nullmod` with `modadmin -Q` refcounts;
- auto-load of a device by open;
- an A/UX binary auto-loading `auxexec`;
- idle auto-unload after `DEF_UNLOAD_DELAY` with `moddaemon` running;
- `cpusha` path on 040 and 060 (a module whose `_load` runs code it just relocated).

---

## 15. Open items

1. ASV `sysent` use of 64–77, 82, 83, 105, 140 (§2.1).
2. `d_close` per `otyp` in AMIX specfs (§4.2); decides the open-set key.
3. `sleep` without `PCATCH` behaviour and `u_qsav` offset (u+0x6f0 per `aux-kernel-design.md`); confirm from `sys/user.h` and `systrap`.
4. `vn_open`/`vn_rdwr` argument lists and `kmem_alloc` flag values from the AMIX headers.
5. `plat_intr_attach` source numbering per platform (platform-interface work).
6. Whether `KSYM_SPACE` should become exact-sized by a two-pass link if the COFF size limit of A/UX Startup becomes tight (the image is 2.23 MB).
