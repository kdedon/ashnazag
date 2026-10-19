# Kernel memory

How the kernel finds virtual space and memory for its own allocations, and what was
changed in the stock AMIX kernel to lift its limits.

## The fixed arena

AMIX gives the kernel heap one fixed 4 MB virtual arena (`kvseg`, at `syssegs`), managed by
`sptmap` through `sptalloc`/`sptfree`. Every multi-page `kmem_alloc`, buffer mapping and
device window comes from it. 4 MB was a limit: at 128 MB of RAM the page array alone took
about 2 MB, and a large swap area left no room for display shadows or environment modules
(#210).

## Address map (68040/68060)

```
0x00000000  RAM, identity mapped (kernel image)
0x40000000  u            current u-area
0x40040000  syssegs      fixed arena, 4 MB (kvseg, sptmap)
0x40440000  kvsegmap     segkmap, 128 MB
0x48440000  kvsegu       u-areas (segu)
   ...      heap chunks  4 MB each, from the first 4 MB boundary above the u-areas
0x50000000  Mac I/O, VA = PA; 0x52800000 Mac ROM alias
0x60000000  cached RAM window, VA = PA | 0x60000000 (Mac, CT60): page array
0x7fffffff  end of the kernel's quadrant
```

## Growing heap

On 68040/68060 kernels (`kernel/vm/kheap.c`) `sptalloc` serves page-backed requests of up to
4 MB from chunks once the arena is short:

- A request goes to the first chunk with room (first fit, one bit per page); else to the
  arena while that keeps `KH_KEEP` pages (1 MB) free for paging I/O and device mappings;
  else to a new chunk. At most 32 chunks (128 MB), ending at 0x7fffffff; growth stops at the first
  4 MB range that already has page tables.
- A new chunk takes one page for its leaf tables: 16 tables of 64 entries, entered in the
  16 pointer descriptors of `kptr040` covering it. Its segment is a copy of `kvseg`, so
  `segkmem` maps and unmaps its pages.
- Requests with a fixed base, or larger than a chunk, go to the stock `sptalloc`. When no
  chunk can be added, a caller that may sleep waits in the stock path for the arena; one
  that may not gets 0.
- `sptfree` returns pages to their chunk, or to the arena, and wakes `kmem_alloc` sleepers.

Chunks are never given back.

## Sleeping kmem_alloc (#226)

The stock `kmem_alloc` with `KM_SLEEP` waits for a free at least as large as the request,
which may never come. Over one page the wrapper now:

- panics with the size if the request is over 4 MB, or larger than RAM less `t_minarmem`:
  it could never be met;
- otherwise retries with `KM_NOSLEEP`, sleeping between tries until a free or one second
  passes. No signal ends the wait: callers may hold locks.

One page or less, and `KM_NOSLEEP` requests, go to the stock code.

## Virtual swap (#210)

`kernel/vm/vswap.c`, on every kernel. Anonymous memory reserves against free RAM plus the
swap disks; a page takes a disk page only when it is first written out.

- Each swap area becomes the backing store of a virtual area (block device row 22, one
  minor per area); the first also counts free RAM.
- `anon_resv` keeps reservations within the disks plus the RAM free now, less 2 MB for the
  pager. Reservations beyond the disks are taken from `availrmem`, so locked and kernel
  pages cannot use RAM promised to anonymous memory.
- A page never written reads as zeros. `swap_free` gives its disk page back once a write in
  flight on it ends. `swap -d` removes a disk's area; the same blocks cannot be added twice.

## Wrapping the stock allocators

The stock functions are binary, so ours replace them at relink (`kernel/vm/build.sh`, called
from each platform's relink script):

1. For each replaced function the base kernel gets `__vm_<name>` at its stock address, and
   `<name>` is made global if local and weakened (objcopy).
2. `vm.o` defines `<name>` strongly; linked over the base, it wins every call, and calls
   `__vm_<name>` for the original.
3. Where the relink already wraps a function (the Amiga's boot-trace mark on `sptalloc`),
   ours takes the name `__amix_<name>` under that wrapper (`VM_CHAIN`).

Replaced: `swapadd`, `swapdel`, `swap_free`, `anon_resv`, `anon_unresv` everywhere;
`sptalloc`, `sptfree`, `kmem_alloc` where the base has `kptr040` (68040/68060).

## Binary patches

Two code sites in stock functions are patched in place. Each script finds its function by
symbol, compares the bytes it expects (with neighbouring instructions as an assertion),
refuses anything else, and is idempotent. Relink runs it on the base, then with `-c` (check
only) on the linked kernel.

- `kernel/mac/patch_kvmpages.py`, Mac (68040) and CT60 kernels: `kvm_init` mapped the page array and
  page hash into the arena with `sptalloc`. The array already sits in reserved physical
  memory, so the call is replaced by its address in the cached RAM window, freeing about
  2 MB of arena at 128 MB.
- `kernel/vm/patch_closef.py`, 68040/68060 kernels: `xclosef` panics ("Bad vp", "Bad
  v_op", "Bad vop_close") unless each pointer is in main store or 0x40000000-0x48ffffff.
  Vnodes from heap chunks lie above that; its three upper bounds become 0x7fffffff (#232).

## Risk

Stock code may hold other range checks that assume kernel data ends at 0x48ffffff. The
`xclosef` check showed up only once the heap was full and new vnodes came from the last
chunk; `t_dlm` now fills the heap with a test module and closes pipes made while it is full.
A panic over a kernel pointer above the u-areas likely points at another such check.

## History

- Earlier: page array out of the arena (`patch_kvmpages.py`).
- 2026-10-18: growing heap and virtual swap (#210, #211).
- 2026-10-18: sleeping `kmem_alloc` over a page waits for frees or panics when it could
  never be met (#226).
- 2026-10-19: `xclosef` accepts vnodes in heap chunks (#232).
