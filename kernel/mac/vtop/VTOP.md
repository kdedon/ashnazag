# vtop and hat_map on the 040 base

Two corrections in the Mac overlay over the port's `unix-040`: `vtop` honours the process it is
given, and `hat_map` leaves the 040 root table alone.

| File | Role |
|---|---|
| `vtop.s` | replaces the port's `vtop`: a nonzero proc means a user address of that proc, at any value |
| `patch_vtop.py` | byte patches on the base: `dma_pageio` clears `b_proc`; `hat_map` never grows the legacy segment table; `i40_on` = 0 |
| `relink-mac.sh.diff` | builds `vtop.o`, weakens the port's `vtop`, runs `patch_vtop.py` on `base.weak` and `-c` on the ELF (040 base only; applied) |
| `guest-hat_map.diff` | removes the `hat_map` wrapper from `kernel/guest/`, which the base now covers (applied) |

## vtop

`vtop(va, p)` gives the physical address a driver copies to or from. The port's `vtop040`
dispatches on the address first:

| p | va | port result |
|---|---|---|
| 0 | < 1 GB | `va` (identity) |
| 0 | ≥ 1 GB | stock kernel walk (`svirtophys`) |
| ≠ 0 | < 1 GB | `va` |
| ≠ 0 | 1–2 GB | kernel walk, proc ignored |
| ≠ 0 | ≥ 2 GB | `uvatopte040(va, p)`, the 040 user walk |

That treats a user address below 2 GB as physical or kernel. Native processes can map there
(stock `valid_usr_range` allows quadrant 0); A/UX processes live there.

### Callers

`R_68K_32` references to `vtop` in the base: these ten, plus our `ramstrategy`.

| Caller | Arguments | Buffers it sees | On the Mac |
|---|---|---|---|
| `prmapin`+0xc | user va, p | `/proc` slow path: `prusrio` after `prfastmapin` declines (page not resident, or a write to a write-protected page) and `as_fault(F_SOFTLOCK)` | live, user address |
| `ramstrategy` (`rd.c`) | `b_addr`, `b_proc` | raw `/dev/rdsk/rd*`: `uiophysio` user buffers; block: buffer cache and page I/O, `b_proc` 0 | raw: live, user address |
| dd `startio`+0x40 | `b_addr`, `b_proc` | raw: `amiga_dma_pageio` bounce buffer (kernel, `B_KERNBUF`, `b_proc` 0), copied with `copyout`/`copyin`; block: `gen_strategy` hands physical page addresses, `b_proc` 0 | live, kernel |
| dd `getsense`+0x10 | sense buffer, 0 | kernel | live, kernel |
| ct `start`+0xee | `b_addr`, `B_KERNBUF` ? 0 : `b_proc` | bounce, as dd | no tape |
| ct `machine`+0x220 | `b_addr`, `b_proc` | bounce | no tape |
| `mmmmap`+0x50 | offset, 0 | `/dev/mem`: physical | live, physical |
| `hdstart`+0x64 | `b_addr`, `b_proc` | `uiophysio` user buffers | A2090, not configured |
| `rundevice`+0x6e | `b_addr`, `b_proc` | floppy | not configured |
| stock Amiga `ramstrategy`+0x6c, +0x88 | `b_addr`, `b_proc` | raw RAM disk | replaced by ours |

Writers of `b_proc` in the image: `uiophysio` (the owner for `UIO_USERSPACE` with `B_PHYS`,
else 0), `amiga_dma_pageio` (clears it on its bounce buffer), `dma_pageio` (copies the request's
into its bounce buffer; unreferenced). `pageio_setup`, `getblk`, `ngeteblk` never set it. So a
nonzero `b_proc` always comes with a user address of that process, except in `dma_pageio`.

### Change

- `vtop.s`: p ≠ 0 → `uvatopte040(va, p)` for every va, page frame + offset, 0 if not resident;
  p = 0 → identity below 1 GB, `vtop_orig` above. Every caller passing a proc locks the page
  first (`uiophysio`: `as_fault(F_SOFTLOCK)`; `prusrio`: the same before `prmapin`).
- `patch_vtop.py` `bproc`: `dma_pageio`+0x60 `movel a3@(56),a2@(56)` → `clrl a2@(56); nop`; its
  bounce buffer is kernel memory.
- `physio` and the drivers are unchanged: `uiophysio` flags user buffers with `B_PHYS` and the
  owner, and every kernel buffer has `b_proc` 0.

The port's `vtop040` diagnostics (`vtop pool`, `VTOPALIAS`, `KERNVA-WITH-PROC`) are dropped with
its body.

## hat_map

Stock `hat_map` (preload already branched out by the port) grows the 030 segment table of
section `s = va >> 30` whenever the segment's last segment number exceeds the table's limit,
stored at root words `8*s` and `8*s+4`, then loads URP with the segment's root. On the 040 those
words are root entries 0–7 (VA 0–0x0fffffff): sections 0/1 would corrupt entries 0–3, sections
2/3 put tables in entries 4–7, which `hat_free040` → `hat_legacy_sdt_free` then reads as legacy
tables to release.

The live `hat_growsdt` callers are `hat_map`, `hat_init` (kernel, at boot) and that teardown;
`hat_devload` goes through `hat_pteload`, and `hat_ptalloc` and stock `hat_dup`/`hat_exec`/
`hat_swapout` are unreachable on the port.

### Change

- `patch_vtop.py` `hatmap`: `hat_map`+0x90 (20 bytes, up to the `jsr hat_growsdt` address word)
  → `seg->s_as == u.u_procp->p_as` ? URP load : return. The URP load is needed for exec's new
  address space, but only for the current process: `segdev_dup` calls `segdev_create` →
  `hat_map` on the child's segment during fork, which would leave the parent running on the
  child's root. `kas` never matches.
- `patch_vtop.py` `legacy`: `i40_on` 1 → 0. `hat_legacy_sdt_free` only counts (`i40_calls`);
  no legacy table exists to free, and entries 4–7 may be live.
- The port image is 040/060 only (`cputype` 40 or 60); the 68030 kernel is the stock one.
  `patch_vtop.py` refuses a base without `cputype` and `kptr040`, and `relink-mac.sh` runs it
  only on a port base, so a stock base keeps the stock `hat_map`.

`patch_vtop.py -c` checks the three patches in the linked ELF.

## Tests

`tests/src/t_vtop.c` (area `vtop`, last in `runall`). Each user buffer sits at the virtual
address equal to the physical address of the RAM-disk block it transfers (`rd_unit[0].base` +
offset, from `/tests/ksyms`), so a kernel that mistakes the address for a physical one only
copies that block onto itself: the test sees the error and nothing else is touched.

| Test | Checks |
|---|---|
| `raw_read_lowva` | 8 KB raw read of `rd0` into three separately allocated low pages equals a normal-buffer read |
| `raw_write_lowva` | 1 KB raw write from a low page reaches the disk (block of a scratch file) |
| `proc_read_lowva` | `/proc` read of a child's untouched `MAP_SHARED` file page at the low address returns the file, not physical memory at that address |
| `proc_read_lowva_resident` | the same, page touched (fast path) |
| `proc_write_lowva` | `/proc` write to a child's read-only-resident `MAP_PRIVATE` page reaches the child's copy and not physical memory |
| `dd_read_lowva` | raw SCSI (dd, target 0) read into a low buffer across a page boundary; skipped without a disk (direct and `--net` boots) |

## Notes for the port

1. `src/vtop040.s`: dispatch on the proc, not the address. `proc != 0` → `uvatopte040` for all
   va; `proc == 0` → identity below 0x40000000, `vtop_orig` above. Every nonzero proc a caller
   passes names the owner of a user buffer (`uiophysio`, `prmapin`); dispatching on the address
   sends low user addresses to physical memory in `/proc` and in raw I/O that does not bounce
   (the stock RAM disk, `hd`).
2. `dma_pageio` (0x20c90 in `unix-040`): clear `b_proc` on the bounce buffer as
   `amiga_dma_pageio` does, so the contract above holds for every buffer.
3. `hat_map` (0xb5870): skip `hat_growsdt`; load URP (0xb58a2) only when `seg->s_as` is the
   current process's (`202e fff4 2079 4000 0730 b0a8 007c 6600 004e 601e`), next to the preload
   patch in `patch_pmmu_040.py`. Skipping the URP load always stops the boot before init;
   loading it unconditionally runs the parent on the child's root after `segdev_dup`. With no
   legacy table created, ship `i40_on` = 0 or drop `hat_legacy_sdt_free`, which otherwise reads
   live 040 root entries 4–7 as legacy tables.
