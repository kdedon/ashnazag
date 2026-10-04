# s5 directory updates on disk

A wrapper for `uiomove` marks every segkmap page it writes into as modified, so s5 directory
updates reach the disk even when the MMU leaves the page's M bit clear.

- `uiomod.c`: the wrapper (calls the stock body as `__amix_uiomove`).
- `relink-mac.diff`: builds it and adds `uiomove` to the overrides and aliases of
  `relink-mac.sh` (applied).

Test: `tests/s5disk/` (1 KB and 2 KB s5 on disk slices; see its README).

## Why

s5 writes directory entries with `rdwri` → `writei`: `uiomove` into the file's page through the
segkmap window, then `segmap_release`. Whether the page is written depends on its modified bit:
`pvn_getdirty` takes it from `p_mod`, or from the PTE's M bit through `hat_pagesync`. Nothing on
this path sets `p_mod`, so the M bit is the only record.

Directory code reads the page just before writing it (`dirsearch` → `fbread`). On a 68040 a
later store through an ATC entry with M clear walks the table again and sets M. An MMU that
sets M only on a walk made for a store (as QEMU's 68040 does) leaves M clear, since the read's
translation already allows writes. The page then looks clean to the synchronous write in
`writei` (`IO_SYNC` → `s5putpage` → `pvn_range_dirty`), to `sync` and to unmount
(`pvn_vplist_dirty`): the entry change stays in memory while the removed file's inode and blocks
are freed on disk, so fsck sees names pointing at free inodes.

ufs writes entries through `fbread`/`fbwrite`, whose `F_SOFTUNLOCK, S_WRITE` sets `p_mod`
explicitly.

## How

`uiomove(cp, n, UIO_WRITE, uio)` with `cp` inside segkmap: after a successful stock copy, for
each 4 KB page of the bytes moved (the `uio_resid` decrease), the window's `smap` gives vnode and
offset, `page_exists` the page, and `p_mod` (bit 0x04 of the first byte, one `orib`, atomic
against the interrupt-level `pvn_done`) is set, as `fbwrite` does for ufs. A failed copy marks
nothing: a page `segmap_pagecreate` made for a full overwrite holds stale memory, which must not
be written to the file. Writes to other kernel buffers and reads go straight to the stock body.

Cost: a few compares for reads and non-segkmap writes; one `page_exists` per page written
through segkmap. On hardware that sets M, pages written by `writei` are already modified, so there is
no extra I/O.

Scope: every `writei`-style write (s5, ufs, spec) goes through it. Kernel stores into segkmap by
other means (`bcopy` in `s5flushsb`, `bzero` in `pvn_vptrunc`) still depend on the M bit; they
write into freshly faulted pages, where the first access is the store.

The link binds `uiomove` to the wrapper at all 44 call sites.

## Open

- `s5flushsb` rewrites the whole 4 KB device page holding the superblock. On a 1 KB s5 that page
  also holds inode blocks 2 and 3 (inodes 1–32), read from disk just before; a concurrent
  `bwrite` of those blocks between the read and the write would be undone. With 2 KB blocks the
  page holds only blocks 0 and 1.
- A full-page `pvn_done` leaves `p_nio` at 1; `s5putpage` sets it again before every write, so it
  has no effect.

## Note for the port

The port's `build/unix-040` has the same `writei`/`uiomove` path, so s5 on it loses directory
updates under such an MMU. The wrapper uses only `segkmap`, `smap` and `page_exists`, nothing
Mac-specific.
