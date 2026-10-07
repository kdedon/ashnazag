# Amiga host: the PCI bus through openpci.library

On an Amiga host the PCI bridges (Elbox Mediator A1200/Z4, Matay Prometheus, DCE G-REX, Firestorm) are driven by Thomas Richter's **openpci.library**, a closed AmigaOS binary. That library already handles each bridge's quirks: probing, BAR placement, bridges behind bridges, interrupt routing and DMA windows. We don't rewrite it. Instead the kernel runs it as an AmigaOS library inside a small emulated exec (**amilib**), and the **opci** DLM module gives Unix modules a C interface to the bus (`<sys/opci.h>`). A PCI driver module names `$depend opci` in its Master file.

```
 PCI driver module ($depend opci)
        | opci_find, opci_cfgread, opci_attr, opci_intr ...   <sys/opci.h>
 opci   (opci/opci.c)      one LVO call each, under one lock
        | am_lvo: AmigaOS register convention (amglue.s)
 openpci.library            loaded from /etc/conf/pci/openpci.library
        | exec / expansion / utility / timer.device LVOs
 amilib (amilib/)           exec, lists, memory, Disable, semaphores, server chains,
        |                   ConfigDevs from the host's Zorro boards, LoadSeg
 amxplat.c                  AMIX: kmem, spl, printf, dlm_cacheflush, plat_* hooks
```

## Files

| File | Role |
|---|---|
| `amilib/amilib.h` | AmigaOS offsets (byte-offset access, `AB`/`AW`/`AL`), the platform interface |
| `amilib/amglue.s` | `am_entry` (library → C), `am_call` (C → library), `am_super` (exec `Supervisor`: a format-0 frame, the function `rte`s), `am_isr` (C interrupt servers) |
| `amilib/amexec.c` | exec: our jump tables, the dispatcher, lists, AllocMem/AllocVec/pools, Disable/Forbid, signals, ports, semaphores, AddIntServer chains, MakeLibrary/MakeFunctions/InitStruct/InitResident/SetFunction, devices, RawDoFmt, caches |
| `amilib/amlibs.c` | utility (tags, hooks, maths, strings), expansion (BoardList, FindConfigDev), timer.device |
| `amilib/amhunk.c` | LoadSeg from a buffer, bounds-checked |
| `opci/opci.c` | `<sys/opci.h>` over the library's LVOs; init, listing and expunge |
| `opci/amxplat.c` | the AMIX platform part |
| `include/sys/opci.h` | the interface for PCI driver modules |
| `mod/opci/` | the DLM wrapper (`MOD_MISC_WRAPPER`), Master, Space.c (library path, delay loops) |
| `mods.sh` | builds `mod.d/opci` for a kernel with DLM, as `atari/mods.sh` |
| `test/` | the emulator harness (below) |

## How the library sees the kernel

- **Calls into our libraries.** Each jump-table entry is `jmp stub`, and each stub is `jsr am_entry`. `am_dispatch` finds the library and LVO from the stub address, not from a6, so `SetFunction` works. It patches the jump-table entry and returns the stub as the old function, which still reaches our handler. An LVO we don't provide is logged once and returns 0.
- **One task, no scheduler.** Forbid and Permit only count. Disable raises the IPL to 7 and Enable restores it. Semaphores never block, because `opci` lets one caller into the library at a time. `Wait` returns what it was asked for. Devices complete requests in `BeginIO`, and `timer.device` busy-waits.
- **Memory** comes from `kmem_alloc`, with `KM_NOSLEEP` under Disable or at interrupt level. Chip memory is refused. `TypeOfMem` reports public fast memory.
- **Zorro boards.** Each board becomes a `ConfigDev` on ExpansionBase's BoardList (+60). `cd_BoardAddr` is the board's kernel mapping, so every address the library derives from it (BARs, legacy IO, config space) is a kernel address. openpci adds and removes ConfigDevs on that list itself.
- **Config cycles.** On a 68010 or later, openpci makes them under `Supervisor()`. Its routine points VBR at a table on the stack, catches the bus error, restores VBR and `rte`s into our frame. This works in the kernel as it does under AmigaOS.
- **Interrupts.** openpci adds its bridge server to the PORTS (level 2) or EXTER (level 6) chain. The first server on a chain asks the host to route that level (`plat_zintr`). The host's handler then runs the chain (`am_intrun`), and the library's server calls the device servers `opci_intr` added.
- **DMA addresses** (`opci_busaddr`). Inside a bridge window the library gets the kernel address it handed out. Main memory goes as a physical address (`plat_vtop`), because without mmu.library the library treats logical and physical addresses as the same.
- **Not provided:** dos.library, so `ENVARC:PCI-Configuration` is never read and the defaults apply; mmu.library, so there is no virtual window for the A1200 Mediators; intuition. openpci runs without all three.

## What the Amiga host layer must supply (with DLM)

Weak references in `amxplat.c`. Without them the module loads, finds no boards and fails with `ENXIO`.

| Hook | Contract |
|---|---|
| `int plat_zorro(int i, struct amx_zboard *zb)` | board `i` of the AMIX autoconfig list (physical base and size, manufacturer, product, `er_Type`, `er_Flags`, serial, diag vector); nonzero past the end |
| `char *plat_iomap(pa, size)`, `void plat_iounmap(va, size)` | supervisor, cache-inhibited (serialized) kernel mapping of a board. Zorro III boards are large: a Prometheus is 512 MB, a Mediator 4000 window up to 512 MB |
| `unsigned long plat_vtop(va)` | physical address of a kernel address |
| `int plat_zintr(int intnum, void (*fn)(int))`, `void plat_zunintr(int)` | call `fn(intnum)` from the level 2 handler (intnum 3) or level 6 handler (13), shared with the stock CIA/Zorro servers; return 0 when done |

From the kernel: `kmem_alloc`, `kmem_free`, `printf`, `sleep`, `wakeup`, `vn_open`, `vn_rdwr`, and `dlm_cacheflush` (DLM). `delayus`, `hrestime` and `cputype` are used when present.

## Tests

```sh
sh kernel/amiga/test/run.sh [openpci.library]     # or OPENPCI=..., or media/openpci.library
```

This builds amilib, opci and a test program with `m68k-linux-gnu-gcc -m68060`, which keeps out the 64-bit multiply the 060 lacks, and runs them on an emulated 68040 (Python `unicorn`). There is one PASS/FAIL line per check:

- **LoadSeg:** code, data and BSS hunks with RELOC32 and RELOC32SHORT. A truncated file, a relocation past its hunk, resident library names and a relocation to a missing hunk are refused. Nothing is leaked.
- **exec and utility:** RawDoFmt, OpenLibrary, GetTagData across TAG_IGNORE, Stricmp, UMult64, Enqueue and FindName. Opening a library we lack fails. Nothing is leaked.
- **openpci.library 40.15, no bridge:** init fails with `ENXIO` and everything is freed.
- **openpci.library 40.15 on a modelled Prometheus** (0xad47/1). Its PCI config space is at +0xF0000 + slot<<13 + fn<<8, little-endian and address-invariant, which is how the library reads it. There are two cards. The library enumerates both, places BAR0 inside the board, enables decoding, and handles config word, long and byte reads and writes, find by IDs, obtain/busy/release, and the second card. An interrupt on its PORTS server reaches the device handler, and nothing reaches it after `opci_unintr`. Unload expunges the library, and amilib frees every byte. No LVO the library calls is missing.

The harness is the only place unicorn appears. Unicorn treats `rte` as an exception, so `run.py` performs it (format 0 and 2 frames).

## Open

1. The `plat_*` hooks in the Amiga host layer, built with DLM on `unix-040`/`unix-060`. `mods.sh` is untested with the AMIX toolchain, since this session had none.
2. Mediator, G-REX and Firestorm are untested; only the Prometheus path has run, on the model.
3. `PCI-Configuration`: a small dos.library (Open/Read/Close/Lock, and ReadArgs as openpci uses it) over the kernel's files would let `/etc/conf/pci/PCI-Configuration` apply.
4. mmu.library, for the A1200 Mediators' virtual window.
5. A `/dev/pci` with an `lspci`, for users. The load already lists the bus on the console.
