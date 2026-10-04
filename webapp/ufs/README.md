# SVR4 UFS generator

`Build(ctx, dst, options, entries)` writes a big-endian filesystem through `io.WriterAt`. File contents come from `io.ReaderAt`; each transfer uses at most 8 KiB. Free-space metadata uses one byte per KiB of filesystem capacity. File contents never need to fit in memory.

The destination must already have the requested length and contain zeroes. A newly created, truncated file works; reusing an existing file requires clearing it first. On failure or cancellation, discard the partial output.

Sizes are 4–2048 MiB in multiples of 4. Geometry matches the native builder: 8 KiB blocks, 1 KiB fragments, and 4 MiB cylinder groups. Entries support directories, files, symlinks, hard links, character/block devices, and FIFOs. Missing parents and `/lost+found` are created automatically. Paths must be absolute and canonical; hard links must target a non-directory, non-link entry. A zero timestamp is deterministic.

Tests compare complete output with the existing Python builder, run its independent consistency checker, and extract a file spanning double-indirect blocks. Additional checks cover multi-group inode allocation, 2 GiB sparse output, cancellation, insufficient space, malformed entries, and short writes.
