# Direct boot from HFS boot blocks

The ROM starts the kernel with no System file and no A/UX Startup:

```
ROM Start Manager ─> Apple_Driver (from the disk, unchanged) ─> boot blocks of the
Apple_HFS partition (ours) ─> _Read of the kernel image ─> trampoline ─> mac_entry
```

## Files

| File | Role |
|---|---|
| `bootblk.s` | The 1024 boot-block bytes: header, parameters, loader, trampoline, command line |
| `mkbb.c` | Host tool (K&R C): `flat` (ELF → flat image), `patch` (find the image in the volume, fill in the parameters), `check` (verify a disk image) |
| `build.sh` | Assembles `build/bootblk.bin` with `m68k-elf-as -m68040` (listing in `build/bootblk.lst`), builds `build/mkbb` |
| `mkboot.sh` | Builds `images/q800-unix.img` (installed as `images/mkboot.sh`) |
| `images.diff` | `images/README.md` section for the image |

## ROM entry conditions (Quadra 800 ROM F1ACAD13)

From the ROM's boot-disk code at offset 0x1350–0x1D40 (addresses below at ROM base 0x40800000).

| Step | Address | What the ROM does |
|---|---|---|
| Boot stack | 0x40800490 | SP = ((`BufPtr` + `SysZone`) / 2, even) − 1024: mid-RAM, not the top |
| Buffers | 0x408002E6 | a6 = SP + 1024 (boot-block buffer, 1024 bytes); a5 = SP + 400; the parameter block for its reads is at a6 − 1024 |
| Drive loop | 0x40801350 | walks the drive queue (`DrvQHdr` $308); for each drive sets `BootDrive` ($210) = dQDrive and $B34 = dQRefNum, also in the parameter block (ioVRefNum +22, ioRefNum +24) |
| Read | 0x40801600, patch 0x408041D2 | `_Read`, ioPosMode 1, offset 0, 1024 bytes into a6; then a hook through $6F4; accepted if the first word is 'LK' |
| Zones | 0x40801D14 | TheZone = ApplZone = SysZone, HeapEnd = its bkLim |
| Run | 0x40801D24 | bbVersion high byte 0x44, or bits 7 and 6 both set → `jsr 2(a6)` (bbEntry). Otherwise, or after the code returns, `_MountVol` (0x40801D52) and the System file |

At `bbEntry`: supervisor, SR 0x2000 (interrupts on), the ROM's MMU and caches as set up
for Mac OS, SP = a6 − 0x404. The boot drive's driver is open and answers `_Read` by drive
number and refnum. The volume is **not** mounted, so the File Manager cannot open a file.
ROM Gestalt is installed (table at ROM offset 0x169AA: `mach`, `ram `, `lram`, …) and
low-memory globals (`BufPtr`, `ScrnBase`, `MainDevice`, …) are set.

## Read method

The kernel is an HFS file (`unix`, the flat image), read by device offset: one `_Read` on
the boot drive (`BootDrive`, refnum from $B34), ioPosMode fsFromStart, offset and length
from the boot-block parameters. `mkbb patch` finds the file's bytes in the volume (block
aligned, one contiguous run) and records the offset and a checksum. Reasons:

- The volume is not mounted; `_MountVol` + `_Open` would not fit in 1024 bytes.
- A flat image needs no ELF parsing: one copy, one BSS clear.
- The file stays visible and checkable (`hfsck`, `hcopy`). Replacing it needs `mkbb patch`
  again; a moved or changed file fails the checksum (error 104) instead of crashing.

One stage: the code ends at 0x360, the command line takes 0x380–0x3FF.

## Boot-block layout

| Offset | Content |
|---|---|
| 0x00 | 'LK', `bra.w start`, bbVersion 0x4418 (header words `4C4B 6000 00E4 4418`) |
| 0x0A–0x8B | rest of the standard header, zero (only read if the code returns) |
| 0x8C | 'UxBB', then image offset in the partition, length (multiple of 512), load address, `end`, entry, checksum (sum of the image's longs) |
| 0xA8 | trampoline (64 bytes, copied out) |
| 0xE8–0x35F | loader |
| 0x380 | command line, NUL-terminated, at most 127 bytes |

## Memory during the load

```
0          SysZone ... HeapEnd      stack <- SP   a6: boot blocks (ROM frames above)
|---------|===========|------------|<------------|---|----...----|
     (mid RAM)                                         0x1000 min gap
... free ...| image (length L) |T: trampoline 64 B|boot record ≤ 512 B| BufPtr | ROM data |
            ^d4                ^d5
```

- T = (`BufPtr` − 64 − 512) & ~15, image = (T − L) & ~15.
- Checks: image ≥ code + 0x1000 (error 101), `end` + 516 ≤ T (error 102).
- The trampoline copies image → load address (0x10000) upwards (source above
  destination, so overlap is safe), clears BSS to `end`, and copies the boot record to
  (`end` + 1) & ~1, where `mac_entry` scans for it.
- Example, L = 0x228E00, `end` 0x281334, 8 MB: stack near 4 MB, image near 5.6–7.8 MB.
  The kernel footprint must stay below the trampoline (about RAM − 1 MB).

## Boot record

Same Linux/m68k tag format as `auxentry.s`, plus depth, size and command line:

| Tag | Source |
|---|---|
| BI_MACHTYPE 3, BI_CPUTYPE/BI_MMUTYPE 68040 | `CPUFlag` $12F must be 4 (error 105) |
| BI_FPUTYPE 68040 | `HWCfgFlags` $B22 bit 12 |
| BI_MEMCHUNK {0, size}, BI_MAC_MEMSIZE | Gestalt `'ram '`, else `MemTop` |
| BI_MAC_MODEL | Gestalt `'mach'` (35 on a Q800), else `BoxFlag` + 6 |
| BI_MAC_ROMBASE, BI_MAC_VIA1BASE, BI_MAC_SCCBASE | `ROMBase` $2AE, `VIA` $1D4, `SCCRd` $1D8 |
| BI_MAC_VADDR, BI_MAC_VROW, BI_MAC_VDEPTH, BI_MAC_VDIM | main GDevice's PixMap (`MainDevice` $8A4 → gdPMap → baseAddr, rowBytes, pixelSize, bounds); else `ScrnBase`, `ScreenRow`, `ChunkyDepth`. A base must lie in 0xF9000000–0xF97FFFFF or 0xFA000000–0xFEFFFFFF; in 24-bit mode (`MMU32Bit` $CB2 = 0) a base outside them goes through `_Translate24To32` and is checked again. A PixMap base that fails is replaced by `ScrnBase`, checked the same way |
| BI_COMMAND_LINE | boot-block offset 0x380, if not empty |

The video mode is the one the ROM booted with (from PRAM). `fbcons_biinit` takes all four
video tags as given.

## Hand-off

Trampoline, at IPL 7 after `cpusha bc`: CACR, TC, ITT0/1, DTT0/1 = 0, `pflusha`, copy,
clear, record, `jmp` entry with d0 = 0, a0 = 0. `mac_entry` treats that as a direct boot,
the same as QEMU `-kernel`; the kernel needs no boot-block-specific code.

Assumption: the ROM maps RAM VA = PA (the trampoline switches translation off while
running). True on the Quadra 800, whose djMEMC makes RAM one range from 0.

## Errors

`_SysError` with the id, then a loop. `DSAlertTab` is nil before the System loads, so the
ROM shows its critical-error screen (Sad Mac): major code 0x0F, minor the id in hex
(0x65–0x69).

| Id | Meaning |
|---|---|
| 101 | no room for the image above the loader |
| 102 | kernel too large for the RAM |
| 103 | `_Read` failed or came short |
| 104 | checksum mismatch (image moved or replaced without `mkbb patch`) |
| 105 | not a 68040 |

## Image

`sh images/mkboot.sh [-c cmdline] [-m MB] [kernel.elf [out.img]]` → `images/q800-unix.img`:

| Blocks | Partition |
|---|---|
| 1–63 | partition map (3 entries), from `q800-test-small.img` |
| 64–95 | Apple_Driver, byte-identical to the A/UX disk |
| 96– | Apple_HFS "MacOS", volume "Unix", sized for the kernel (4 MB): boot blocks + file `unix` |

`mkboot.sh` checks: `hfsck` on the volume and the image, `auxsash.py apmcheck` (driver and
map entries as in the source), `unix` read back equals the flat kernel, `mkbb check`
(header, bbVersion, parameters against the ELF, image bytes on disk, in allocation blocks
marked used in the volume bitmap). `mkbb patch` only accepts a match in used blocks.

## Testing

- `build.sh`: the output must be exactly 1024 bytes; inspect `build/bootblk.lst`.
- `mkboot.sh` runs the image checks above.
- Boot the image under QEMU q800: it should reach the single-user shell with the screen
  console (24-bit mode reports video base 0xF9001000, depth 8, row 0x400, 640×480).

## Addressing mode

PRAM byte 0x8A bit 2 selects 32-bit mode at startup (the ROM copies it to bit 0 of $1EFC);
the result is `MMU32Bit` $CB2. In 24-bit mode `BufPtr` is at most 8 MB, so the image and
trampoline stay below 8 MB whatever RAM is installed; `BI_MEMCHUNK` still reports all of it
(Gestalt `'ram '` is physical).
