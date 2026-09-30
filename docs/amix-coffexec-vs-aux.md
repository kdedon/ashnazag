# AMIX COFF exec vs. A/UX 3.1 executables

Analysis of the AMIX 2.1 relink kit (`usr/sys/exec/exp`, `os/exp`, `vm/exp`, `master.d/kernel.c`) against A/UX 3.1 binaries and the A/UX kernel's own loader (`/unix`: `gethead`, `loadshlibs`, `getxfile`, `execbld`, `setregs`). Offsets in AMIX functions are `.text` offsets of the named object. *(uncertain)* marks inferences that were not traced to the end.

Tools: `tools/coffhdr.py` dumps COFF headers and `.lib` entries, and replays the AMIX acceptance checks (`-r <root>` follows the `.lib` paths). `tools/elfdisr.py` disassembles a byte range of an ELF relocatable and decodes jump tables.

## Verdict

AMIX loads and maps the text, data and bss of every A/UX `0413` executable. It never loads A/UX shared libraries. It ignores the A/UX `.lib` section, so the first call into `libc1_s`/`libmac1_s` faults. This affects 625 of the 682 `F_EXEC` COFF files on the image. Even with the `.lib` format fixed, A/UX puts every shared library at 0x47c00000–0x47fc8000. AMIX refuses all user mappings in 0x40000000–0x7fffffff. So running A/UX shared-library programs needs a kernel change, whichever loader (COFF or converted ELF) is used.

| Subject | Loads as-is? | Blocking differences |
|---|---|---|
| `/bin/sh` (0413, no `.lib`) | **Yes** | None at exec. After that, the syscall/signal ABI applies (not covered here). |
| `/mac/bin/startmac` | Loads, then SIGSEGV at the first `bsr.l $47f024d4` (`startmac`+0xa) | `.lib` is ignored (B1, B2). The libraries fail AMIX checks (B3, B4). Library addresses are in quadrant 1 (B5). |
| `/mac/bin/CommandShell`, `/mac/bin/launch`, `Login System Folder/Login` | Same as startmac | Same |
| `/shlib/libc1_s`, `/shlib/libmac1_s` | Can't be `exec`ed, which is expected. As libraries they fail B3, B4 and B5. | |

## 1. What AMIX accepts

`execsw[]` (in `master.d/kernel.c`, which is source) = `{coffmagic 0x0150, coffexec, coffcore}`, `{elfmagic, elfexec, elfcore}`, `{intpmagic "#!", intpexec}`. `exec/exp` is always linked. `SHLBMAX` = 2 (`master.d/kernel.h:64`, `shlbinfo = {SHLBMAX,0,0,0}`).

### `getcoffhead(vp, exdata*, npages*, exhda*)` (static, exec/exp 0x27a)

- Reads the 20-byte filehdr. Requires `f_magic == 0x150` and `F_EXEC` (0x0002). Otherwise `ENOEXEC`. `f_flags` is not checked further, so `F_AR32WR` and the rest are ignored. No machine check.
- If `f_opthdr >= 28`, it reads `aouthdr.magic` into `ux_mag` and `aouthdr.entry` into `ux_entloc`. No other a.out field is used; sizes and addresses come from the section headers.
- Section loop. It matches `s_flags` **exactly**:
  - `0x20` TEXT: sets `txtorg=s_vaddr`, `toffset=s_scnptr`, `tsize`. With no a.out header it forces `ux_mag=0x108` and `entry=s_vaddr`.
  - `0x40` DATA: sets `datorg`, `doffset`, `dsize`.
  - `0x80` BSS: sets `bsize` only. The bss `s_vaddr` is ignored; bss is placed at `datorg+dsize`.
  - `0x800` STYP_LIB: `ux_nshlibs = s_paddr` (checked against `shlbmax`, otherwise `ELIBMAX`), `lsize = s_size`, `loffset = s_scnptr`.
  - Any other flag value, including `0x22`/`0x42`/`0x82` (NOLOAD|x), `0x41` (DSECT|DATA) and `0x200` (INFO), is **silently ignored**. A later section with the same exact flag overwrites an earlier one.
- It needs all three of TEXT, DATA and BSS (`0xe0`), otherwise `ENOEXEC`. Every exec object pays for all three.
- It checks `sum(ceil(size/2K))` against an rlimit in the u-area (u+0x7d4, `ENOMEM`). This is the only place 2 KB pages are hard-coded in exec/exp (`0x7ff`, `>>11`). It's harmless on a 4 KB kernel.

### `coffexec(vp, uarg*, level, npages*, exhda*)` (exec/exp 0x0)

- Magic switch (jump table at 0x5c, cases 0x108–0x123): **0x108 (0410), 0x109 (0411) and 0x10b (0413) are accepted.** 0x123 (0443, shared library) gives `ELIBEXEC`. Anything else, **including 0x107 (0407)**, gives `ENOEXEC`.
- If `ux_nshlibs` (taken from `.lib` `s_paddr`) is non-zero, it calls `getcoffshlibs`. The buffer has room for `nshlibs` entries of 0x34 bytes (on the stack if ≤ 2).
- `remove_proc(uarg)` builds the new stack and address space (below). Then, for each library and then the program:
  `execmap(vp, txtorg, tsize, 0, toffset, 0xd /*R|X|U*/)` and `execmap(vp, datorg, dsize, bsize, doffset, 0xf /*RWX|U*/)`. A failure sends SIGKILL.
- `setexecenv({brkbase = datorg+dsize+bsize, magic = 0x150, vp})`. It copies `exdata` to `u_exdata` (u+0x8bc). The entry point is `ux_entloc`.
- No address-range, alignment or overlap checks are made here.

### `getcoffshlibs(vp, exdata*, shlib[], npages*, exhda*)` (exec/exp 0x47e)

- Maps the `.lib` section and walks it in SVR3 format. Each entry is `{long n_words; long pathoff_words; char path[]}`. The walk continues while `p < start + (lsize & ~3)`. An entry needs `2 < n`, `4n <= lsize` and `pathoff <= lsize`, otherwise `ELIBSCN`. Zero padding at the end of the section is therefore an error.
- `lookupname(path, UIO_SYSSPACE, FOLLOW)`. It then needs `VOP_GETATTR`, `VOP_ACCESS(VEXEC)`, `v_type == VREG` and a mode with an x bit. Any failure gives `ELIBACC`.
- It runs `getcoffhead` on the library, so the library needs **`F_EXEC`** and text+data+bss. It then needs **`ux_mag == 0x123`**. Anything else gives `ELIBBAD`, or `ENOMEM` passed through.
- It doesn't bound the count by `shlbmax`. The caller's buffer is sized from `s_paddr` (coffexec) or `shlbmax` (ELF path), so `s_paddr` must equal the number of entries.

### `execmap(vp, addr, len, zfodlen, off, prot)` (os/exp 0x1bc14)

- If `(off & 0x7ff) == (addr & 0x7ff)` and the vnode is mappable, it calls `VOP_MAP(... MAP_PRIVATE|MAP_FIXED)` at page-truncated addr/off. Otherwise it falls back to anonymous zfod memory plus `vn_rdwr` (a copy). Misalignment is therefore **not** fatal; the pages are just not shared. The 040/060 port has to make this 4 KB.
- zfod: it clears from `addr+len` to the end of the page (`uvbzero`) and maps the rest as zfod at the next page.

### Address-space limits (vm/exp)

- **`valid_usr_range(addr,len)` (vm/exp 0x773e) rejects any user range in quadrant 1 (0x40000000–0x7fffffff), and any range that crosses a 1 GB boundary.** `seg_alloc` calls it for every non-kernel `as` (0xae54), so `as_map` → `execmap`/`mmap`/`shmat` fail there.
- The kernel runs in its own supervisor space: `pmove` to SRP in ml/exp 0x100a, and `copyin` uses `moves` with SFC=1. `hat_alloc` gives each process a private 4-entry root table, all entries invalid. The HAT code (`hat_pteload`, `hat_exec`) handles every quadrant the same way (`addr>>30`, 13-bit B index). The kernel's own dynamic VA is in quadrant 1 (`u` 0x40000000, `syssegs` 0x40040000, `kvsegmap` 0x40440000, `kvsegu` 0x48440000, from `amiga/ml/syms.o`). *(uncertain)*: the exclusion looks like policy mirroring the kernel layout rather than a hardware need. Relaxing `valid_usr_range` may be enough, but no user of quadrant-1 user addresses was checked.
- User stack: `extractarg` fixes `stackend = 0xc0800000` (UVSTKBASE) for every process. The u-block is at 0xc0000000, the default mmap/shm search starts at 0xc1000000 (`map_addr`), and `UVEND` is 0xf1000000.

### ELF path (for an offline conversion)

- `getelfhead` (0xc3e): `ELFCLASS32`, `ELFDATA2MSB`, `e_machine == 4`, `e_type` ET_EXEC or ET_DYN, `e_phentsize != 0`. If `e_flags` bit 0 is set, an FPU is required.
- `mapelfexec` (0xd14): for each `PT_LOAD`, `execmap(vaddr, filesz, memsz-filesz, offset, prot)`, where prot is 8 plus PF_R→1, PF_W→2, PF_X→4. `PT_SHLIB` (type 5) is remembered. `PT_PHDR` without `PT_INTERP` causes a failure (`elfexec` 0x93a).
- **`elf_coffshlib` (0xe7c): the contents of the `PT_SHLIB` segment are handed to `getcoffshlibs`.** An ELF executable can therefore pull in COFF static shared libraries, with the same `.lib` format, `F_EXEC`/0x123 and quadrant rules.

### Process start (`setregs` os/exp 0x1cd2a)

- `sp` points at `argc, argv[], 0, envp[], 0`, then the strings. There's no auxv for COFF, since `coffexec` never sets `auxsize`. This is the same layout A/UX `execbld` builds, and A/UX `crt0` (`_start`) reads it correctly.
- `PC = u_exdata.ux_entloc`. **`D0 = sp`** (`u_ar0[0]` and `u_ar0[15]` are both set). *(uncertain)*: A/UX `setregs` zeroes the registers in `regloc`, so D0 is 0 there. A/UX `crt0` stores D0 in `splimit%`. No other reference to `splimit%` was found in `sh`/`startmac`, so this looks harmless.
- The FPU is initialised if present. Other user registers are not explicitly cleared *(uncertain)*.

## 2. What the A/UX files contain

All of them have `f_magic 0x150` and `f_flags 0x203` (RELFLG|EXEC|AR32WR), with a 28-byte a.out header of magic `0x10b` (0413). The text section's file offset equals its offset from a 0x10000000 base (Mac programs) or from 0 (`sh`), so the whole file is mapped from offset 0. Data is congruent with its file offset mod 8 KB (startmac: 0x100416e0 / 0xb6e0).

| File | text vaddr / size / off | data vaddr / size / off | bss | entry | `.lib` |
|---|---|---|---|---|---|
| startmac | 0x100001e8 / 0xb4f8 / 0x1e8 | 0x100416e0 / 0x1ec4 / 0xb6e0 | 0x100435a4 / 0x3aa8 | 0x100001ee | libmac1_s, libc1_s |
| CommandShell | 0x100001e8 / 0x28658 / 0x1e8 | 0x10040840 / 0x5f3c / 0x28840 | 0x1004677c / 0xde2c | 0x100001ee | same |
| launch | 0x100001e8 / 0xcbf8 / 0x1e8 | 0x10040de0 / 0x20b4 / 0xcde0 | 0x10042e94 / 0x3ba8 | 0x10001e1e | same |
| Login | 0x100001e8 / 0x12388 / 0x1e8 | 0x10040570 / 0x267c / 0x12570 | 0x10042bec / 0x46bc | 0x1000557e | same |
| sh | 0xa8 / 0x9c68 / 0xa8 | 0x401d10 / 0x1834 / 0x9d10 | 0x403544 / 0x1298 | 0xa8 | none |

The Mac programs also have `.lt*`/`.ld*`/`.lb*` sections with flags 0x22/0x42/0x82 (NOLOAD|TEXT/DATA/BSS, `scnptr` 0). These are the libraries' layout; the kernel ignores them on both systems. `.lowmem` is flags 0x41 (DSECT|DATA), size 0, vaddr 0. `.lib` is **flags 0x200 (STYP_INFO)**, `s_paddr 0`, size 0x80.

**A/UX `.lib` format:** a flat array of **64-byte NUL-padded path names**, with count = `s_size/64` (for example `/shlib/libmac1_s`, `/shlib/libc1_s`). There's no header word. Across the image: 541 executables use `libc1_s`, 47 `libX11_s+libc_s`, 14 `libuucp_s+libc1_s`, 13 `libc_s`, 8 `libmac1_s+libc1_s` and 2 `libX11_s+libc1_s`. None uses more than 2, so SHLBMAX 2 is enough.

**A/UX shared libraries** (`/shlib/*`): `f_flags 0x200` or `0x100`, **with F_EXEC clear**. The a.out magic is **0x108 (0410)**, not 0443. They still carry relocations. Text starts at file offset 0xa8/0xd0, not congruent with its vaddr, so AMIX would copy it rather than map it (A/UX's `loadshlibs` also uses `loadreg`, a copy, for 0410).

| Library | text | data | bss end |
|---|---|---|---|
| libuucp_s | 0x47c00000 +0x26e1c | 0x47cc0000 +0x14dd4 | 0x47cdc070 |
| libX11_s | 0x47d00000 +0x1ef08 | 0x47dc0000 +0x7d34 | 0x47dc8f54 |
| libmac1_s | 0x47e00000 +0xda90 | 0x47ec0000 +0x3324 | 0x47ec3be0 |
| libc1_s | 0x47f00000 +0x73f8 | 0x47fc0000 +0x244c | 0x47fc24e0 |

The executables are stripped of relocations and call the libraries through absolute addresses (startmac: `jmp $47e02024`, `bsr.l $47f024d4`, `move.l $47ec2ce0,…`). The libraries therefore **can't be moved** without rewriting every executable.

Across the image, `coffhdr.py`'s replay rejects only three executables: the two `0x107` Patch files (`Patch.0178`, `Patch.067C`, loaded in user space by `__tb_coff_load`, never `exec`ed) and `/etc/config.d/newunix`. `startmac24`/`CommandShell24`/`cmdo24` are statically linked `0x108` with `.low24`. They load, but need 24-bit semantics (below).

### How A/UX's own kernel loads them (for comparison)

- `gethead` (0x1001621a) accepts `0x150`, `#!`, and **AppleSingle/AppleDouble files (0x00051600/0x00051607), which it runs via `/mac/bin/launch`**. It dispatches on the exact section flags: 0x20, 0x40, 0x80, **0x41 by name** (`.lowmem` sets the exec flag 4, `.low24` sets flag 2), and **0x200 → shared-library count = `s_size>>6`, offset = `s_scnptr`**.
- Accepted a.out magics: 0x108, 0x10b and 0x107 (0407, text folded into data).
- Stack top: `.low24` → 0x01000000. **`.lowmem` → 0x40000000.** Otherwise 0, meaning the end of the address space *(inferred)*. `chksize` checks the image fits below top−256 KB. startmac's stack therefore grows down from 0x40000000.
- `loadshlibs` (0x1001675a) reads 64-byte records. For each library it needs `0x150`, text+data+bss, and magic 0x108/0x10b. F_EXEC is not required. Shared text goes through `xalloc`. The data region is attached at `data_vaddr & ~0x3ffff`, with `mapreg` for 0x10b and `loadreg` for 0x108, and bss is grown.
- `setregs` (0x10007280): all registers are cleared, `SP` is the argument block and `PC` is the entry.
- `.lowmem` itself carries no data. Mac RAM at 0 is the `tLOW` shm segment that `libmac1_s` attaches at 0. The ROM goes at 0x40800000, which is also quadrant 1.

## 3. Blocking differences

- **B1** `.lib` is STYP_INFO (0x200), and AMIX only recognises flags == 0x800. `s_paddr` is 0 where AMIX expects the count.
- **B2** The `.lib` contents are 64-byte path records. AMIX expects `{n, off, path}` records and rejects trailing padding.
- **B3** The libraries have F_EXEC clear, and AMIX `getcoffhead` requires it.
- **B4** The library a.out magic is 0x108, and AMIX requires 0x123.
- **B5** The library addresses 0x47c00000–0x47fc8000 fall in quadrant 1. `valid_usr_range` → `seg_alloc` fails, so `execmap` fails and the process gets SIGKILL. The same limit blocks libmac's later ROM mapping at 0x40800000.
- Not blocking, but different:
  - AMIX places the stack top at 0xc0800000; A/UX uses 0x40000000 (`.lowmem`) or 16 MB (`.low24`).
  - 0407 executables are refused.
  - AppleSingle/AppleDouble files aren't run via `launch`.
  - D0 at entry is sp on AMIX, 0 on A/UX.
  - Text is mapped R-X (A/UX 0410/0413 text is read-only too).
  - 24-bit address masking for `.low24` programs is not available.

## 4. What a fix needs

**Kernel side, in every variant:** allow user segments in quadrant 1, at least 0x40800000–0x4fffffff, which covers the ROM and all libraries. The minimum is to override `valid_usr_range` (a global in vm/exp, so weaken-and-override works). *(uncertain)*: check that nothing else assumes user addresses ≥ 0x40000000 are unused, such as `as_gap`/`map_addr` search bounds, grow/stack checks and the HAT segment-table allocator. The kernel's own quadrant-1 VA is in a separate SRP space, so no overlap is expected.

Then pick one of the following.

**(a) Kernel COFF loader for A/UX.** `execsw` is in `kernel.c`, so add an entry, or replace `coffexec`/`getcoffshlibs` (both global). `getcoffhead` is static, so the easiest route is a full replacement of `exec/exp`'s COFF part. It must:
- Accept a `.lib` section by name or with flags 0x200, count = `size/64`, and read fixed 64-byte paths.
- Accept libraries with F_EXEC clear and magic 0x108/0x10b.
- Accept 0407 if it's ever needed.
- Detect `.lowmem`/`.low24` (flags 0x41, by name) to mark A/UX Mac processes, and to choose the stack top if that turns out to matter. `extractarg` hard-codes 0xc0800000, so that would be a second override.
- Optionally add an AppleSingle/AppleDouble execsw entry (first short 0x0005) that runs `/mac/bin/launch`, the way `intpexec` does.

This leaves A/UX files untouched.

**(b) Offline COFF→COFF patch (no new loader code).**
- Rewrite each executable's `.lib`: set flags to 0x800, `s_paddr` to the number of libraries, and the contents to SVR3 records. `/shlib/libmac1_s` is 8+17 bytes rounded up = 7 words. The last record's `n` must absorb the rest of the 0x80 bytes, or `s_size` must be shrunk.
- Set the libraries' F_EXEC and change their a.out magic to 0x123.

AMIX's `coffexec` then loads everything, B5 aside. The patched files no longer load on A/UX: A/UX needs 0x200 for `.lib` and 0x108/0x10b for libraries.

**(c) Offline COFF→ELF.**
- ET_EXEC, EM_68K, `e_flags` 0.
- PT_LOAD text R+X at `0x10000000`, offset 0.
- PT_LOAD data RW with memsz covering bss.
- No PT_PHDR.
- Either:
  - a PT_SHLIB segment holding SVR3 `.lib` records, which reuses `getcoffshlibs`, so the libraries need the (b) header patches; or
  - fold each library's text/data/bss into the executable as extra PT_LOADs at their fixed addresses. That's static binding: no sharing, about 100–400 KB per file.

It needs the same quadrant-1 change. It gains nothing over (b) for loading, but it fits a toolchain that is ELF-native.

**Start-up items outside the loader:**
- The syscall and signal ABI.
- Stack top (0xc0800000 vs 0x40000000). Unknown whether libmac depends on it.
- D0/`splimit%`.
- The `tLOW` shm attach at 0 (quadrant 0, allowed).
- The ROM at 0x40800000 (quadrant 1, B5).
- 24-bit mode for `*24` programs.
