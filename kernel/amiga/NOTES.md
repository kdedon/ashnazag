# Amiga host: the PCI bus through openpci.library

On an Amiga host the PCI bridges (Elbox Mediator A1200/Z4, Matay Prometheus, DCE G-REX, Firestorm) are driven by Thomas Richter's **openpci.library**, a closed AmigaOS binary. That library already handles each bridge's quirks: probing, BAR placement, bridges behind bridges, interrupt routing and DMA windows. We don't rewrite it. Instead the kernel runs it as an AmigaOS library inside a small emulated AmigaOS, the **amilib** DLM module. The **opci** module (`$depend amilib`) starts it and gives Unix modules a C interface to the bus (`<sys/opci.h>`). A PCI driver module names `$depend opci` in its Master file. Any later wrapper for another Amiga library depends on amilib the same way.

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

`am_init` and `am_fini` are counted. The environment (ExecBase, the libraries, the ConfigDevs) is built for its first user and taken down after its last. The amilib module refuses to unload while any user holds it.

## How the library sees the kernel

- **Calls into our libraries.** Each jump-table entry is `jmp stub`, and each stub is `jsr am_entry`. `am_dispatch` finds the library and LVO from the stub address, not from a6, so `SetFunction` works. It patches the jump-table entry and returns the stub as the old function, which still reaches our handler. An LVO we don't provide is logged once and returns 0.
- **Its own stack.** At process level, every call into the library runs on a 16 KB stack of amilib's, not the caller's kernel stack. openpci's call chains are deep: a 516-byte line buffer while parsing configuration, a 128-byte vector table while probing, and exec callbacks nested inside both. Calls at interrupt level (server chains) stay on the interrupt stack. Only one process is ever inside, under amilib's lock (`amx_lock`), so a sleep in `kmem_alloc` there is safe.
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

The real mmu.library (MMULib 47) cannot run in the kernel: through 68040.library and 68060.library it takes over the MMU, which AMIX owns. amilib offers an mmu.library of its own instead (`ammmu.c`). It owns no tables and is an adapter: what a library asks of the MMU goes to the kernel, through the `plat_*` interface below, and the kernel's mappings stay the authority. LVO offsets are from `MMU_lib.fd` 41.1.

It reports **version 43**, and openpci then uses it like this:

| Call | Here |
|---|---|
| `DefaultContext`, `SuperContext`, `Lock`/`UnlockMMUContext`, `Lock`/`UnlockContextList` (and the Attempt forms) | two contexts; locks only count |
| `GetPageSize` | the kernel's page size (`plat_pagesize`, else 4096) |
| `GetMMUType` | from AttnFlags: 68030, 68040 or 68060 |
| `GetMapping`, `ReleaseMapping`, `SetPropertyList` | an empty description handed back and forth |
| `SetPropertiesA` | accepted for ranges inside a mapped board, and those stay as the kernel mapped them (cache-inhibited, serialized). Refused, and logged, anywhere else. openpci's property words are not interpreted further |
| `GetPropertiesA` | 0 |
| `RebuildTree`, `RebuildTreesA` | success |
| `WithoutMMU` | the function runs in supervisor state with the MMU still on; openpci only uses it to wrap its bus-error-safe config probe, which catches faults itself |

The context windows of V46, which openpci uses for the A1200 Mediators' virtual window (LVOs -426 to -450, a context exception hook through -168, -192 and -174, and private contexts through -114 and -120), are not provided. Seeing version 43, openpci maps those boards physically, so fewer devices fit in their small window.

To add the windows later, the adapter would map them onto two more kernel hooks:

| Hook | For |
|---|---|
| `int plat_remap(va, pa, len, mode)` | point a page of the window at another bridge bank |
| `int plat_faulthook(va, len, fn)` | a supervisor access fault inside the range calls `fn(va)`, which remaps and asks for the access to be retried |

The other side, what openpci expects of the V46 calls and of its hook's exception data, needs the V46 autodocs and includes (`mmu/context.h`, `mmu/mmutags.h`, `mmu/exceptions.h`), or a reading of mmu.library 47's own code.

## What the Amiga host layer must supply (with DLM)

Weak references in `amilib/amxplat.c`. Without them both modules load, but opci finds no boards and fails with `ENXIO`.

| Hook | Contract |
|---|---|
| `int plat_zorro(int i, struct amx_zboard *zb)` | board `i` of the AMIX autoconfig list (physical base and size, manufacturer, product, `er_Type`, `er_Flags`, serial, diag vector); nonzero past the end |
| `char *plat_iomap(pa, size)`, `void plat_iounmap(va, size)` | supervisor, cache-inhibited (serialized) kernel mapping of a board. Zorro III boards are large: a Prometheus is 512 MB, a Mediator 4000 window up to 512 MB |
| `unsigned long plat_vtop(va)` | physical address of a kernel address |
| `unsigned long plat_pagesize()` | the MMU page size (absent: 4096) |
| `int plat_zintr(int intnum, void (*fn)(int))`, `void plat_zunintr(int)` | call `fn(intnum)` from the level 2 handler (intnum 3) or level 6 handler (13), shared with the stock CIA/Zorro servers; return 0 when done |

From the kernel: `kmem_alloc`, `kmem_free`, `printf`, `sleep`, `wakeup`, `vn_open`, `vn_rdwr`, and `dlm_cacheflush` (DLM). `delayus`, `hrestime` and `cputype` are used when present.

## Open

1. The `plat_*` hooks in the Amiga host layer, built with DLM on `unix-040`/`unix-060`. Nothing here has been built with the AMIX toolchain or run on hardware yet. The C was checked with a modern m68k gcc against stand-in headers only.
2. mmu.library context windows for the A1200 Mediators' virtual window (above).
3. A `/dev/pci` with an `lspci`, for users. The load already lists the bus on the console.
