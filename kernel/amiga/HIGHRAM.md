# RAM above the kernel's region (Zorro III RAM as main memory)

AMIX describes memory as one contiguous range, `MAINSTORE` to `MAINSTORE + VSIZOFMEM`, and the kernel reaches every page at its physical address: the low gigabyte is mapped one to one by DTT0 (supervisor, noncacheable), `pagezero`, `ppcopy` and the page-table walkers dereference `pfn << 12`. Zorro III RAM sits at 0x40000000 or above, where kernel virtual region 1 (u-area, kvseg, segmap, u-areas, heap chunks) lives, so it cannot be reached that way.

## What ties RAM to one region and to phys == virt

| Where | What |
|---|---|
| `config()`, `amiga_bootmem` | one region: the one the kernel is loaded into, adjacent fast regions joined |
| `mlsetup` | `physmem = VSIZOFMEM >> 12`, `maxclick = MAINSTORE >> 12 + physmem`: one range of page frames |
| `kvm_init` | `pages[]` covers `[v, maxclick)` contiguously (60 bytes a page); `smsegs` (segmap) grows with RAM: 512 segments of 256 KB fill 0x40440000 to kvsegu, more overlaps the u-areas (the 128 MB cap, `MAXMEM` in amigazorro.c) |
| `kvm_init` → `sptalloc` | the page array and hash mapped into kvseg, a fixed 4 MB arena: 272 MB needs 4.3 MB |
| DTT0 `0x003fa060` | identity for 0 to 1 GB; Zorro III RAM is above it |
| `pagezero`, `ppcopy`, `pagecopy`, the HAT's table walks, `hat_pt2ptdat` | physical address used as a kernel address |
| leaf PTEs, table descriptors, URP | must hold the bus address |
| `vtop(va, 0)` | identity below 1 GB: a device's DMA address |
| `vatosde` | region 1 only; a second region outside `kptr040` panics there (stock AMIX) |

## Design: pseudo-physical frames

RAM outside the kernel's region gets *pseudo-physical* addresses directly above it, so the kernel still sees one contiguous range and every identity access in the link kit keeps working. On the Maximig: RAM 0x07000000 to 0x08000000 (16 MB, kernel), Zorro III RAM 0x40000000 to 0x50000000 shows at 0x08000000 to 0x18000000; `VSIZOFMEM` becomes 272 MB.

```
 kernel view (frames, identity VA)        bus
 0x07000000 +----------------+          0x07000000  motherboard RAM
            | kernel, pages[]|
 0x08000000 +----------------+  DTT0 ends (0x003f -> 0x0007 mask)
            | pseudo frames  | --supervisor page tables, NC--> 0x40000000+  Zorro III RAM
 0x18000000 +----------------+
```

1. **Extents.** `hr_seg[]`: up to 4 `{pseudo, bus, pages}`. `hr_k2p(pa)` and `hr_p2k(pa)` translate; both are one compare when no extent exists. Ordinary frames and device frames are never in either range, so the functions are safe on any address.
2. **Boot (Amiga, `amiga_quiesce`, MMU off).** Fast regions above the kernel's become extents when the kernel's region ends on a power-of-two boundary (DTT0's granularity) and no board or other region decodes the pseudo range or the cached view. Boot modules move to the top of the last extent (`dlm_bootpack` there, then their addresses rebased to pseudo); below them go supervisor pointer and leaf tables for the pseudo range (PTE `bus | 0xF9`: supervisor, noncacheable) and for a cached view of the kernel's region (`pa | 0x99 | hat_cm_ram`) on the next 32 MB boundary above the pseudo range. `VSIZOFMEM` ends below the tables.
3. **MMU on (`vstart` wrapper).** The root's entries from E to 1 GB are cleared, then point at those tables; DTT0 and ITT0 narrow to [0, E) (`0x0007a060` and `0x0007a000` on the Maximig); `pflusha`.
4. **Sizing.** `seg_alloc` of kvsegmap is clamped to `kvsegu - kvsegmap`, so segmap stays 128 MB and stops overlapping kvsegu (`smsegs` only sizes the inert 030 tables). The page array goes to the cached view instead of kvseg (`sptalloc` with a base frame while `pages == 0`, `kh_alias`). The 128 MB cap stays only without extents.
5. **Translation sites.** Everything that hands an address to the MMU or a bus master translates pseudo to bus; everything that reads one back translates bus to pseudo:
   - `hat_pteload`: the frame argument at entry; the old frame in `Lreplace` and the frame compare.
   - Table descriptors: every `table | 2` store (root, pointer, leaf tables from `hat_ptalloc`), the URP value (`hat_alloc`'s 030 root descriptor), and every `desc & ~0x1ff` / `desc & ~0xff` dereference (hat040, hat_dup040, hat_chgprot040, hat_pagesync040, bp_map040, uvatosde040, ptest040, prfastmap040, prumap040, krnxmemflt040, segu_ubptbl040).
   - PTE → frame readers that look up `pages[]` (`cmpl pages_base` sites in hat040, hat_dup040, hat_chgprot040, ptest040, prfastmap040) and PTE writers composing `pfn << 12` (bp_map040, prumap040, hat_dup040 COW copies).
   - `cpushl`/`cpushp` take a bus address: every push of an identity address (hat040's table and frame pushes, dma_cache040, cb_release040, ide040, dlm_cache) translates it. A missed push leaves a stale copyback line from a page's last user mapping that can land on a page table later.
   - `segkmem_alloc`, `segkmem_free`, `segkmem_mapin`, `segkmem_mapout`: replaced (kernel/vm/kseg.c), translating as they write and read kvseg PTEs; `kh_add`'s descriptors.
   - `svirtophys` returns the bus address; `vtop(va, 0)` below 1 GB, `prmapin`, `tblalloc` (amigazorro.c) translate.
   - `resume` finds both u-area leaves before it rewrites the u-area's PTEs, so the stack's page never changes under its translation calls.
   - `plat_iomap`: boards between the narrowed DTT0 and 1 GB go through `dev_kvmap`; `plat_kmemlen` knows the pseudo range.
   - PIO drivers that take `vtop`'s address as a kernel address translate it back (amigaide).
   - guestcore: the process tables' descriptors and frames.
   - bigram: skips boards holding an extent.

The helpers and `hr_seg[]` live in the port (`port-highram.diff`, `src/highram040.s`), so every 040/060 kernel links them; the Mac and Atari fill `hr_seg[]` from their own memory lists later (Mac chunks, TT/FastRAM) with the same boot steps.

## Limits

- Extents must lie where the identity map does not reach: the kernel's region must end on 16 MB × 2^n (Amiga RAM layouts do), and the pseudo range must stay below 1 GB: about 900 MB of extra RAM on the Maximig.
- The kernel's identity view of extra RAM is noncacheable, as DTT0 is for the rest; user mappings are copyback.
- `pages[]` takes 60 bytes a page from the kernel's region: 4.3 MB of the Maximig's 16 MB at 256 MB extra.
- A buffer is DMA- or PIO-contiguous only within one extent; the stock A2090/A2091 bounce copies use the bus address as a kernel address and fail on extent pages.
- The ISSUE-10 diagnostics (`i10rev040.s`, dormant) walk tables without translating.
