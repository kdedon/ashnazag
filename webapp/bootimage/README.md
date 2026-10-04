`Build` creates a deterministic HFS volume containing a contiguous `unix` file,
patches the boot blocks, and copies the donor's Apple driver into a new partition
map. No HFS data or boot code is copied from the donor.

Inputs: a big-endian m68k executable, an Apple disk with one driver at block 64,
and a command line of at most 127 bytes. The generated boot disk stays below
32 MiB; root and swap are added separately by `diskimage.Assemble`.

The HFS formatter writes a bitmap, primary/alternate MDB, empty extents tree,
and a catalog with the volume, root thread, and kernel file. Allocation blocks
are 1024 bytes. Volume timestamps are fixed for reproducible output.

`bootblocks.go` embeds this repository's `kernel/mac/bootblk/bootblk.s`, assembled
by `kernel/mac/bootblk/build.sh`. Regenerate with:

```sh
python3 webapp/bootimage/generate.py
```

The template test detects changes when a native assembly is present. With local
Quadra media, run independent HFS, boot-block, and extracted-kernel checks:

```sh
cd webapp
ASH_BOOT_INTEGRATION=1 go test ./bootimage
```
