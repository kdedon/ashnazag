# Amiga host: the PCI bus through openpci.library

Design and rationale: [docs/amiga-pci-design.md](../../docs/amiga-pci-design.md). This file covers the implementation.

On an Amiga host the PCI bridges (Elbox Mediator A1200/Z4, Matay Prometheus, DCE G-REX, Firestorm) are driven by Thomas Richter's **openpci.library**, a closed AmigaOS binary. That library already handles each bridge's quirks: probing, BAR placement, bridges behind bridges, interrupt routing and DMA windows. We don't rewrite it. Instead the kernel loads it and calls it directly. The **amilib** DLM module is a shim of C functions answering the exec, expansion, utility and other calls the library makes. No AmigaOS runs: no Kickstart, no scheduler, no tasks. The **opci** module (`$depend amilib`) starts it and gives Unix modules a C interface to the bus (`<sys/opci.h>`). A PCI driver module names `$depend opci` in its Master file. Any later wrapper for another Amiga library depends on amilib the same way.

```
 PCI driver module ($depend opci)
        | opci_find, opci_cfgread, opci_attr, opci_intr ...   <sys/opci.h>
 opci   (opci/opci.c)      one LVO call each, under one lock
        | am_lvo: AmigaOS register convention (amglue.s)
 openpci.library            loaded from /etc/conf/pci/openpci.library
        | exec / expansion / utility / timer.device / dos / mmu / intuition LVOs
 amilib module (amilib/)    exec, lists, memory, Disable, semaphores, server chains,
        |                   ConfigDevs from the host's Zorro boards, LoadSeg,
        |                   read-only files and ReadArgs from /etc/conf/pci,
        |                   mmu.library as an adapter onto the kernel's MMU
 amilib/amxplat.c           AMIX: kmem, spl, printf, dlm_cacheflush, plat_* hooks
```

## Files

| File | Role |
|---|---|
| `amilib/amilib.h` | AmigaOS offsets (byte-offset access, `AB`/`AW`/`AL`), the platform interface |
| `amilib/amglue.s` | `am_entry` (library → C), `am_call` (C → library), `am_super` (exec `Supervisor`: a format-0 frame, the function `rte`s), `am_isr` (C interrupt servers) |
| `amilib/amexec.c` | exec: our jump tables, the dispatcher, lists, AllocMem/AllocVec/pools, Disable/Forbid, signals, ports, semaphores, AddIntServer chains, MakeLibrary/MakeFunctions/InitStruct/InitResident/SetFunction, devices, RawDoFmt, caches |
| `amilib/amlibs.c` | utility (tags, hooks, maths, strings), expansion (BoardList, FindConfigDev), timer.device |
| `amilib/amhunk.c` | LoadSeg from a buffer, bounds-checked |
| `amilib/amdos.c` | dos: read-only files, locks, CurrentDir, LoadSeg, RDArgs, ReadArgs/FreeArgs, StrToLong, IoErr, Delay; intuition `DisplayAlert` to the console |
| `amilib/ammmu.c` | mmu.library V43 as an adapter onto the kernel's MMU (below) |
| `amilib/amxplat.c` | the AMIX platform part, and the lock its users share |
| `mod/amilib/` | the amilib module: wrapper, Master, Space.c (delay loops) |
| `opci/opci.c` | `<sys/opci.h>` over the library's LVOs; init, listing and expunge |
| `include/sys/opci.h` | the interface for PCI driver modules |
| `mod/opci/` | the opci module (`$depend amilib`): wrapper, Master, Space.c (configuration directory) |
| `mods.sh` | builds `mod.d/amilib` and `mod.d/opci` for a kernel with DLM, as `atari/mods.sh` |

`am_init` and `am_fini` are counted. The shim's data (an ExecBase, the jump tables, the ConfigDevs) is built for its first user and taken down after its last. The amilib module refuses to unload while any user holds it.

## How the library sees the kernel

- **Calls into our libraries.** Each jump-table entry is `jmp stub`, and each stub is `jsr am_entry`. `am_dispatch` finds the library and LVO from the stub address, not from a6, so `SetFunction` works. It patches the jump-table entry and returns the stub as the old function, which still reaches our handler. An LVO we don't provide is logged once and returns 0.
- **Its own stack.** At process level, every call into the library runs on a 16 KB stack of amilib's, not the caller's kernel stack, unless that stack is already taken (`am_stkbusy`, set from the switch to the return, sleeps included). In that case, for instance an MMU fault in a window while another process sleeps inside amilib, the call stays on its own stack. openpci's call chains are deep: a 516-byte line buffer while parsing configuration, a 128-byte vector table while probing, and exec callbacks nested inside both. Calls at interrupt level (server chains) stay on the interrupt stack. Only one process is ever inside, under amilib's lock (`amx_lock`), so a sleep in `kmem_alloc` there is safe.
- **One process, no scheduler.** The library runs as a Process; dos keeps `pr_Result2` and `pr_CurrentDir` in it, and openpci sets `pr_WindowPtr` there. Forbid and Permit only count. Disable raises the IPL to 7 and Enable restores it. Semaphores never block, because `opci` lets one caller into the library at a time. `Wait` returns what it was asked for. Devices complete requests in `BeginIO`, and `timer.device` busy-waits.
- **Memory** comes from `kmem_alloc`, with `KM_NOSLEEP` under Disable or at interrupt level. Chip memory is refused. `TypeOfMem` reports public fast memory.
- **Zorro boards.** Each board becomes a `ConfigDev` on ExpansionBase's BoardList (+60). `cd_BoardAddr` is the board's kernel mapping, so every address the library derives from it (BARs, legacy IO, config space) is a kernel address. openpci adds and removes ConfigDevs on that list itself.
- **Config cycles.** On a 68010 or later, openpci makes them under `Supervisor()`. Its routine points VBR at a table on the stack, catches the bus error, restores VBR and `rte`s into our frame. This works in the kernel as it does under AmigaOS.
- **Interrupts.** openpci adds its bridge server to the PORTS (level 2) or EXTER (level 6) chain. The first server on a chain asks the host to route that level (`plat_zintr`). The host's handler then runs the chain (`am_intrun`), and the library's server calls the device servers `opci_intr` added.
- **DMA addresses** (`opci_busaddr`). Inside a bridge window the library gets the kernel address it handed out. Main memory goes as a physical address (`plat_vtop`), because without mmu.library the library treats logical and physical addresses as the same.
- **Files.** Every AmigaOS name means the file named by its last component in `/etc/conf/pci` (`opci_confdir`), so nothing outside that directory can be named. That covers `ENVARC:PCI-Configuration` (openpci tries `DEVS:`, `ENV:` and `ENVARC:` in turn, and all three resolve to the same file) and plugins it loads for unknown configuration commands. `LIBS:PCI` resolves to the directory itself, because the directory is known by its own name. Files are read-only and read whole, at most 256 KB.
- **ReadArgs** follows AmigaDOS: aliases, `/A /K /S /N /T /M /F`, `KEY=value`, quoted strings with `*` escapes, and `/M` giving its last words to later `/A` arguments. openpci uses it for each `PCI-Configuration` line.
- **Configuration errors.** openpci reports them with intuition `DisplayAlert`, and those lines go to the console as `alert: ...`.
- **Nothing writes files.**

## mmu.library

The real mmu.library (MMULib 47) cannot run in the kernel: through 68040.library and 68060.library it takes over the MMU, which AMIX owns. amilib has its own (`ammmu.c`). It owns no tables and is an adapter: a library's MMU requests go to the kernel through `amx_*`/`plat_*`, and the kernel's mappings stay the authority.

It reports **version 46** when the kernel has the window hooks (`plat_winrange`, `plat_remap`, `plat_faulthook`/`plat_unfaulthook`), and **43** otherwise. openpci only uses context windows from 46 on, so without the hooks it keeps to the physical mapping.

| Call (LVO) | Here |
|---|---|
| `DefaultContext` (-150), `SuperContext` (-144), `CreateMMUContextA` (-114), `DeleteMMUContext` (-120) | the two home contexts and any private ones; each keeps the ranges described on it |
| `Lock`/`Unlock` context and context list, Attempt forms | counters |
| `GetPageSize` (-48), `GetMMUType` (-54) | `plat_pagesize`; from AttnFlags |
| `GetMapping` (-36), `ReleaseMapping` (-42), `SetPropertyList` (-228) | MappingNodes: the window range as the one blank region (`MAPP_BLANK`), the rest mapped |
| `SetPropertiesA` (-84) | page-aligned. Home contexts: board ranges are accepted and left as the kernel mapped them; `MAPP_WINDOW` ranges (with `MAPTAG_WINDOW`) must lie in the window range and are bound to their window. Private contexts: ranges recorded, with `MAPTAG_DESTINATION` when `MAPP_REMAPPED` |
| `GetPropertiesA` (-90) | the recorded properties, else 0 |
| `RebuildTree`/`RebuildTreesA` (-96, -360) | apply every window |
| `WithoutMMU` (-270) | the function runs in supervisor state with the MMU on |
| `CreateContextWindow` (-426), `DeleteContextWindow` (-432) | a window: its home context and the contexts it can show |
| `BuildContextWindow` (-438), `LayoutContextWindow` (-450) | apply the window as it stands |
| `SetContextWindow` (-444) | which context the window shows; applied at once, at any IPL |
| `AddContextHookA` (-168), `RemContextHook` (-174), `ActivateException` (-192), `DeactivateException` (-198) | exception hooks, by priority |

Applying a window means: for each range of the supervisor context bound to the window (the kernel runs in supervisor state), what the shown context maps there goes to `plat_remap`. A remapped range goes to its destination, translated from a board's kernel address to its physical one; anything else becomes no page.

An access fault in the window range comes through `plat_faulthook`. amilib builds an ExceptionData and runs the active hooks, as mmu.library's dispatcher (LVO -396) does: a0 the data, a1 and a4 the hook data, a6 MMUBase, and a hook that returns 0 (Z set) has repaired the fault. The fields set are +0 task, +4 context, +16 first faulting byte, +20 last byte, +40 flags (2: write), +52 size and +128 MMUBase.

### How openpci uses it on an A1200 Mediator

All of this is from openpci.library 40.15's code.

1. At init, with mmu.library in LibList, openpci takes the default and supervisor contexts and looks in `GetMapping` for the largest blank range above 16 MB that stays inside one 512 MB block (0xEC8). It gives that range to the board's routine (+112), which places the PCI memory window there. The PCI addresses equal those 68K addresses.
2. It cuts the range into slices the size of the Mediator's window (one or two windows; board +148 to +152, shift +324). It creates three private contexts: an invalid one (props `0x01004000`), and bank 0 and bank 1, which remap each slice to the window's board address (`0x20208058` with `MAPTAG_DESTINATION`). For every slice it makes a context window in each home context showing those three, and marks the slice `MAPP_WINDOW` there.
3. It adds an exception hook (0x1436, priority 32) and activates it.
4. On a fault in the range, the hook switches the previous slice to the invalid context and the faulting one to a bank context (`SetContextWindow` on both home contexts). It then sets the Mediator's bank register through the board's routine (+116) and returns 0, so the access is retried.

With the hooks in place, every A1200 Mediator access to a slice not currently shown faults once, then goes to the window. That costs one MMU fault per bank change, as on AmigaOS.

The mmu.library facts used (LVOs -426 to -450, the tags, the hook node, the MappingNode, the ExceptionData fields, the dispatcher's calling convention) come from mmu.library 47.11. Earlier offsets come from `MMU_lib.fd` 41.1. The names of -438, -444 and -450 are ours.

## What the Amiga host layer must supply (with DLM)

Weak references in `amilib/amxplat.c`. Without them both modules load, but opci finds no boards and fails with `ENXIO`.

| Hook | Contract |
|---|---|
| `int plat_zorro(int i, struct amx_zboard *zb)` | board `i` of the AMIX autoconfig list (physical base and size, manufacturer, product, `er_Type`, `er_Flags`, serial, diag vector); nonzero past the end |
| `char *plat_iomap(pa, size)`, `void plat_iounmap(va, size)` | supervisor, cache-inhibited (serialized) kernel mapping of a board. Zorro III boards are large: a Prometheus is 512 MB, a Mediator 4000 window up to 512 MB |
| `unsigned long plat_vtop(va)` | physical address of a kernel address |
| `unsigned long plat_pagesize()` | the MMU page size (absent: 4096) |
| `int plat_winrange(&lo, &hi)` | a kernel VA range kept unmapped for mmu.library windows: page-aligned, in [16 MB, 2 GB), inside one 512 MB-aligned block; 16 MB-aligned and 64 MB or more suits openpci |
| `int plat_remap(va, pa, len, mode)` | map whole pages of that range to `pa` cache-inhibited and serialized (`AMX_MAP_IO`), or unmap them (`AMX_MAP_INVALID`); flushes the ATC; callable at any IPL, including from inside a fault |
| `int plat_faulthook(lo, hi, fn)`, `void plat_unfaulthook(lo, hi)` | a supervisor access fault at `va` in `[lo, hi]` calls `fn(va, len, write)`: 0 means repaired, retry the access; anything else goes to the kernel's own fault handling |

The four window hooks come as a set. Without all of them, mmu.library stays at version 43.
| `int plat_zintr(int intnum, void (*fn)(int))`, `void plat_zunintr(int)` | call `fn(intnum)` from the level 2 handler (intnum 3) or level 6 handler (13), shared with the stock CIA/Zorro servers; return 0 when done |

From the kernel: `kmem_alloc`, `kmem_free`, `printf`, `sleep`, `wakeup`, `vn_open`, `vn_rdwr`, and `dlm_cacheflush` (DLM). `delayus`, `hrestime` and `cputype` are used when present.

## Open

1. The `plat_*` hooks in the Amiga host layer, built with DLM on `unix-040`/`unix-060`. Nothing here has been built with the AMIX toolchain or run on hardware yet. The C was checked with a modern m68k gcc against stand-in headers only.
2. The window hooks in the Amiga host layer, and a first run on an A1200 Mediator, the only bridge that uses them.
3. A `/dev/pci` with an `lspci`, for users. The load already lists the bus on the console.
