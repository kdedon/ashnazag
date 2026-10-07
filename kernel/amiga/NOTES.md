# Amiga host: the PCI bus through openpci.library

On an Amiga host the PCI bridges (Elbox Mediator A1200/Z4, Matay Prometheus, DCE G-REX, Firestorm) are driven by Thomas Richter's **openpci.library**, a closed AmigaOS binary. That library already handles each bridge's quirks: probing, BAR placement, bridges behind bridges, interrupt routing and DMA windows. We don't rewrite it. Instead the kernel runs it as an AmigaOS library inside a small emulated exec (**amilib**), and the **opci** DLM module gives Unix modules a C interface to the bus (`<sys/opci.h>`). A PCI driver module names `$depend opci` in its Master file.

```
 PCI driver module ($depend opci)
        | opci_find, opci_cfgread, opci_attr, opci_intr ...   <sys/opci.h>
 opci   (opci/opci.c)      one LVO call each, under one lock
        | am_lvo: AmigaOS register convention (amglue.s)
 openpci.library            loaded from /etc/conf/pci/openpci.library
        | exec / expansion / utility / timer.device / dos / intuition LVOs
 amilib (amilib/)           exec, lists, memory, Disable, semaphores, server chains,
        |                   ConfigDevs from the host's Zorro boards, LoadSeg,
        |                   read-only files and ReadArgs from /etc/conf/pci
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
| `amilib/amdos.c` | dos: read-only files, locks, CurrentDir, LoadSeg, RDArgs, ReadArgs/FreeArgs, StrToLong, IoErr, Delay; intuition `DisplayAlert` to the console |
| `opci/opci.c` | `<sys/opci.h>` over the library's LVOs; init, listing and expunge |
| `opci/amxplat.c` | the AMIX platform part |
| `include/sys/opci.h` | the interface for PCI driver modules |
| `mod/opci/` | the DLM wrapper (`MOD_MISC_WRAPPER`), Master, Space.c (configuration directory, delay loops) |
| `mods.sh` | builds `mod.d/opci` for a kernel with DLM, as `atari/mods.sh` |

## How the library sees the kernel

- **Calls into our libraries.** Each jump-table entry is `jmp stub`, and each stub is `jsr am_entry`. `am_dispatch` finds the library and LVO from the stub address, not from a6, so `SetFunction` works. It patches the jump-table entry and returns the stub as the old function, which still reaches our handler. An LVO we don't provide is logged once and returns 0.
- **Its own stack.** At process level, every call into the library runs on a 16 KB stack of amilib's, not the caller's kernel stack. openpci's call chains are deep: a 516-byte line buffer while parsing configuration, a 128-byte vector table while probing, and exec callbacks nested inside both. Calls at interrupt level (server chains) stay on the interrupt stack. Only one process is ever inside, under the opci lock, so a sleep in `kmem_alloc` there is safe.
- **One process, no scheduler.** The library runs as a Process; dos keeps `pr_Result2` and `pr_CurrentDir` in it, and openpci sets `pr_WindowPtr` there. Forbid and Permit only count. Disable raises the IPL to 7 and Enable restores it. Semaphores never block, because `opci` lets one caller into the library at a time. `Wait` returns what it was asked for. Devices complete requests in `BeginIO`, and `timer.device` busy-waits.
- **Memory** comes from `kmem_alloc`, with `KM_NOSLEEP` under Disable or at interrupt level. Chip memory is refused. `TypeOfMem` reports public fast memory.
- **Zorro boards.** Each board becomes a `ConfigDev` on ExpansionBase's BoardList (+60). `cd_BoardAddr` is the board's kernel mapping, so every address the library derives from it (BARs, legacy IO, config space) is a kernel address. openpci adds and removes ConfigDevs on that list itself.
- **Config cycles.** On a 68010 or later, openpci makes them under `Supervisor()`. Its routine points VBR at a table on the stack, catches the bus error, restores VBR and `rte`s into our frame. This works in the kernel as it does under AmigaOS.
- **Interrupts.** openpci adds its bridge server to the PORTS (level 2) or EXTER (level 6) chain. The first server on a chain asks the host to route that level (`plat_zintr`). The host's handler then runs the chain (`am_intrun`), and the library's server calls the device servers `opci_intr` added.
- **DMA addresses** (`opci_busaddr`). Inside a bridge window the library gets the kernel address it handed out. Main memory goes as a physical address (`plat_vtop`), because without mmu.library the library treats logical and physical addresses as the same.
- **Files.** Every AmigaOS name means the file named by its last component in `/etc/conf/pci` (`opci_confdir`), so nothing outside that directory can be named. That covers `ENVARC:PCI-Configuration` (openpci tries `DEVS:`, `ENV:` and `ENVARC:` in turn, and all three resolve to the same file) and plugins it loads for unknown configuration commands. `LIBS:PCI` resolves to the directory itself, because the directory is known by its own name. Files are read-only and read whole, at most 256 KB.
- **ReadArgs** follows AmigaDOS: aliases, `/A /K /S /N /T /M /F`, `KEY=value`, quoted strings with `*` escapes, and `/M` giving its last words to later `/A` arguments. openpci uses it for each `PCI-Configuration` line.
- **Configuration errors.** openpci reports them with intuition `DisplayAlert`, and those lines go to the console as `alert: ...`.
- **Not provided:** mmu.library (below). Nothing writes files.

## mmu.library

The real mmu.library (MMULib 47) cannot run in the kernel: through 68040.library and 68060.library it takes over the MMU, which AMIX owns. openpci uses it in two places.

- **Every bridge.** On the first open, openpci makes the bridge windows cache-inhibited in the default and supervisor contexts (`GetMapping`, `SetPropertiesA`, `RebuildTreesA`, `SetPropertyList`, `ReleaseMapping`). In the kernel `plat_iomap` already maps boards cache-inhibited and serialized, so nothing is lost without it.
- **A1200 Mediators, `VirtualMapping` (mmu v46+).** openpci creates three private contexts (LVO -114), fills indirect descriptors (-426, -432, -438, -450) and installs a context exception hook (-168, -192, -174). The hook moves the Mediator's window bank when an access faults inside the 68K window. Without mmu.library openpci maps those boards physically, so fewer devices fit in the small window.

Emulating that second part means modelling the needed mmu.library calls on AMIX's page tables and its supervisor access-fault path, through further `plat_*` hooks. It needs the exact API: tag values and indirect-descriptor semantics, which are in the MMULib developer archive (Aminet `dev/c/MMULib.lha`, with the autodocs and `mmu_lib.fd`).

## What the Amiga host layer must supply (with DLM)

Weak references in `amxplat.c`. Without them the module loads, finds no boards and fails with `ENXIO`.

| Hook | Contract |
|---|---|
| `int plat_zorro(int i, struct amx_zboard *zb)` | board `i` of the AMIX autoconfig list (physical base and size, manufacturer, product, `er_Type`, `er_Flags`, serial, diag vector); nonzero past the end |
| `char *plat_iomap(pa, size)`, `void plat_iounmap(va, size)` | supervisor, cache-inhibited (serialized) kernel mapping of a board. Zorro III boards are large: a Prometheus is 512 MB, a Mediator 4000 window up to 512 MB |
| `unsigned long plat_vtop(va)` | physical address of a kernel address |
| `int plat_zintr(int intnum, void (*fn)(int))`, `void plat_zunintr(int)` | call `fn(intnum)` from the level 2 handler (intnum 3) or level 6 handler (13), shared with the stock CIA/Zorro servers; return 0 when done |

From the kernel: `kmem_alloc`, `kmem_free`, `printf`, `sleep`, `wakeup`, `vn_open`, `vn_rdwr`, and `dlm_cacheflush` (DLM). `delayus`, `hrestime` and `cputype` are used when present.

## Open

1. The `plat_*` hooks in the Amiga host layer, built with DLM on `unix-040`/`unix-060`. Nothing here has been built with the AMIX toolchain or run on hardware yet. The C was checked with a modern m68k gcc against stand-in headers only.
2. mmu.library emulation for the A1200 Mediators' virtual window (above).
3. A `/dev/pci` with an `lspci`, for users. The load already lists the bus on the console.
