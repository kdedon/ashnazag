# PCI on the Amiga host: openpci.library in the kernel

How the kernel gets a PCI bus on an Amiga with a Zorro or clock-port PCI bridge (Elbox Mediator A1200/Z4, Matay Prometheus, DCE G-REX, Firestorm). We don't write a bridge driver for each board. The kernel loads Thomas Richter's **openpci.library**, the AmigaOS driver for all of them, and calls it directly. It answers the few dozen exec, expansion and utility calls the library makes with a thin shim of C functions, and gives Unix modules a C interface to the bus. No AmigaOS runs: no Kickstart, no scheduler, no tasks, nothing in the background. §4.5 gives what this costs.

Status: code in `kernel/amiga/` (modules `amilib` and `opci`). Compiled for m68k against stand-in headers only. Not built with the AMIX toolchain, not run on hardware: the Amiga host layer (the `plat_*` hooks, §6) and DLM on the Amiga kernel are still to come. Implementation notes: [kernel/amiga/NOTES.md](../kernel/amiga/NOTES.md).

Inputs: openpci.library 40.15 (openpci68k archive), MMULib 47.11 (mmu.library, CPU libraries, `PCIInit`), the MMULib 41.1 vbcc stubs (`MMU_lib.fd` LVOs), the openpci includes and guide, [dlm-spec.md](dlm-spec.md). Facts the documentation lacks were read from the binaries (§8).

## 0. Summary

| Item | Decision |
|---|---|
| Bridge support | **Reuse openpci.library** unchanged: one closed AmigaOS binary covers every bridge's probing, BAR placement, PCI-to-PCI bridges, interrupt routing and DMA windows |
| How it runs | in the kernel, called through **amilib**: a shim of C functions answering the calls openpci makes into exec, expansion, utility, dos, mmu, intuition (one call) and timer.device. No AmigaOS runs |
| Modules | `amilib` (MISC: the shim and the AMIX glue) and `opci` (MISC, `$depend amilib`: starts openpci, exports `<sys/opci.h>`). PCI drivers are modules with `$depend opci` |
| Interface for drivers | `<sys/opci.h>`: find by IDs or class, config read/write, attributes (BARs, IRQ, IDs), claim/release, bus mastering, regions, DMA memory, bus addresses, interrupt handlers |
| The library file | read at load from `/etc/conf/pci/openpci.library`, never shipped. `PCI-Configuration` and plugins come from the same directory |
| mmu.library | the real one cannot run here (it would take the MMU from AMIX). amilib's is an **adapter**: contexts, windows and exception hooks kept as bookkeeping, mappings done by the kernel through `plat_remap`/`plat_faulthook` |
| A1200 Mediators | their virtual PCI window (mmu.library V46 context windows) is supported when the kernel supplies the window hooks; otherwise mmu.library reports V43 and openpci maps the boards physically |
| Kernel contract | weak `plat_*` hooks from the Amiga host layer: Zorro board list, board mapping, vtop, page size, interrupt chain routing, and the four window hooks |
| Licensing | openpci.library and mmu.library are not redistributed; only facts about their interfaces are used |

## 1. Problem

The Amiga PCI bridges have nothing in common below the PCI side:

| Bridge | Host bus | Config cycles | PCI memory as seen by the 68K | Interrupts |
|---|---|---|---|---|
| Prometheus | Zorro III | memory-mapped window at board + 0xF0000, address-invariant byte lanes | direct, inside the board | INT2 |
| Mediator Z4 (A3000/A4000) | Zorro III | through register banks | large direct window | INT2/INT6 |
| Mediator A1200 | clock port / Zorro II-like | banked | small **banked window**; the rest by bank switching | INT2 |
| G-REX | CyberStorm/Blizzard PPC local bus | own protocol, parity | direct; DMA into CPU RAM possible | own |
| Firestorm | local bus | extended config cycles, bridges behind bridges | direct | own |

None of these has a NetBSD or Linux driver to port ([driver-reuse.md](driver-reuse.md) covers Mac and Atari only). The documentation of the bridges is thin, and some behaviour exists only in Elbox's and DCE's firmware notes and in openpci.library itself. That library is maintained (40.15, 2026), handles all five, and has been tested by its author on real boards. Its release notes record years of quirk fixes: bank closing on the Mediator A1200, G-REX parity and reset, bridge limits, the 50 to 100 ms waits after bridge setup. Writing our own drivers would mean rediscovering all of that without the hardware at hand.

## 2. Options considered

| Option | For | Against |
|---|---|---|
| Own driver per bridge | no foreign code in the kernel | five drivers from little documentation; every quirk relearned; untestable here |
| Run openpci in an AmigaOS guest (the Amiga environment, [guest-container-design.md](guest-container-design.md)) | uses an AmigaOS we already host | Unix drivers would depend on a running guest; interrupts and DMA would cross a process boundary; the guest is a user of the bus, not its owner |
| Rewrite openpci's logic by hand | native code | a large job; every library update means doing it again |
| **Run openpci.library in the kernel, on a shim for the calls it makes (chosen)** | the real, maintained binary; updates drop in; the shim is small because a library needs a few dozen calls, not an OS | an ABI layer to maintain; the library is closed, so its needs are found by logging its calls |

The shim stays small because openpci is a library and not a program. It needs exec (memory, lists, semaphores, interrupt servers, library and device plumbing), expansion (to find its boards), utility (tags), a timer for delays, dos only for its configuration file, and mmu.library only for cache properties and the A1200 window. It needs no scheduler, no message-passing between tasks, no graphics, and no file system beyond reading one directory.

## 3. Architecture

```
  PCI driver module ($depend opci)
        |  opci_find, opci_cfgread, opci_attr, opci_intr ...      <sys/opci.h>
  opci module             one LVO call per operation, under amilib's lock
        |  am_lvo: AmigaOS register convention (amglue.s)
  openpci.library         the user's file, loaded as LoadSeg would
        |  exec / expansion / utility / dos / mmu / intuition / timer.device LVOs
  amilib module           the shim: jump tables answering openpci's calls (memory,
        |                 lists, Disable, semaphores, server chains, ConfigDevs,
        |                 LoadSeg, read-only files, ReadArgs, mmu adapter, hooks)
  amxplat.c               AMIX: kmem, spl, printf, vnodes, dlm_cacheflush
        |  plat_* (weak)
  Amiga host layer        Zorro autoconfig list, board mappings, INT2/INT6 routing,
                          vtop, page size, window range, remap, fault hook
```

| Module | Contents | Depends on |
|---|---|---|
| `amilib` | `amexec.c` (exec), `amlibs.c` (utility, expansion, timer.device), `amhunk.c` (LoadSeg), `amdos.c` (dos, intuition DisplayAlert), `ammmu.c` (mmu.library), `amglue.s` (register convention), `amxplat.c` (AMIX glue, the lock) | kernel with DLM |
| `opci` | `opci.c`, the `<sys/opci.h>` calls; starts and expunges openpci | `amilib` |

amilib is its own module so that a later wrapper of another Amiga library depends on it in the same way. `am_init` and `am_fini` are counted: the shim's data (an ExecBase, the jump tables, the ConfigDevs) exists while at least one user holds it, and the module refuses to unload before then.

## 4. amilib

### 4.1 Calling convention

AmigaOS passes arguments in registers, a6 holds the library base, and functions sit at negative offsets (LVOs) from the base. amilib bridges that to C in both directions with three short assembly routines:

- **Library → us.** Each of our jump-table entries is `jmp stub`, and each stub is `jsr am_entry`. `am_entry` saves d0–a6 into a frame and calls `am_dispatch`. That works out the library and LVO from the stub's address, not from a6, and runs the C handler on the frame. All registers come back from the frame, so the AmigaOS rule that d2–d7/a2–a6 are preserved holds, and the CCR is set from d0. Because the stub, not the jump-table entry, identifies the call, `SetFunction` can patch the table and still return a callable old function (the stub).
- **Us → library.** `am_call(fn, regs)` loads all registers, calls, and stores them all back. RawDoFmt's PutChProc advancing a3 is one caller that needs every register.
- **`Supervisor()`.** `am_super` pushes a format-0 exception frame and enters the function, which leaves by `rte`. openpci uses this for its bus-error-safe config probe: it points VBR at a vector table on the stack, catches the fault and restores VBR. That works unchanged in the kernel, which already runs in supervisor state.

Unprovided LVOs log once and return 0, so a new library version shows on the console what it needs before it can crash on it.

### 4.2 Execution model

- **One process, no scheduler.** The library sees a single Process (Forbid only counts, Wait never blocks, devices finish requests in BeginIO, semaphores never contend). This holds because amilib's lock lets one caller in at a time.
- **Disable** raises the IPL to 7 and Enable restores it, nesting as exec does.
- **Its own stack.** At process level every call into the library runs on a 16 KB stack of amilib's. openpci's call chains are deep (a 516-byte line buffer while parsing configuration, a 128-byte vector table while probing, nested callbacks), and a per-process kernel stack is not sized for them. A busy flag keeps a second caller from using that stack while the first is inside, asleep or not. A second caller is either a fault in an mmu window or an interrupt; it stays on its own stack.
- **Memory** comes from `kmem_alloc`, without sleeping under Disable or at interrupt level. Chip memory is refused.
- **Interrupts.** `AddIntServer` keeps exec-style server chains. The first server on a chain asks the host to route that Zorro level to us, and the host's handler runs the chain (`am_intrun`). A server that returns non-zero ends the chain, as in exec.

### 4.3 Boards

Each Zorro board from the host's autoconfig list becomes a `ConfigDev` on ExpansionBase's private BoardList (+60). openpci adds and removes ConfigDevs on that list itself, so `FindConfigDev` walks the real list. `cd_BoardAddr` is the board's **kernel** mapping, so every address the library derives from it (BARs, config space, legacy IO) is directly usable by kernel code. `opci_busaddr` gives the library each address in the form it expects: kernel addresses inside a board, physical addresses for main memory.

### 4.4 Files

dos.library is read-only and knows one directory, `/etc/conf/pci`. Every AmigaOS name (`ENVARC:PCI-Configuration`, `LIBS:PCI/x`, a bare plugin name) means the file named by its last component in that directory. The directory itself answers to its own name (`LIBS:PCI`). Nothing outside it can be named. ReadArgs follows AmigaDOS, since openpci parses each configuration line with it. Configuration errors arrive as intuition `DisplayAlert` text and go to the console.

### 4.5 What it costs

| | Size or cost |
|---|---|
| amilib code and data | about 27 KB (cross-compiled, `-O2`; the AMIX build will differ a little) |
| opci | about 3.5 KB |
| openpci.library | about 27 KB, loaded once |
| amilib's call stack | 16 KB, reserved while amilib is in use |
| Running in the background | nothing: no threads, no timers, no polling |

Where time goes:

| Path | amilib involved? |
|---|---|
| A driver's register and memory accesses (BARs) | **no**: plain loads and stores to the board's kernel mapping |
| DMA to and from PCI-side memory | no |
| Bus setup (enumeration, BAR placement, `PCI-Configuration`) | once, at `modload opci` |
| Config space reads and writes | one call through the register glue (some tens of instructions) plus openpci's own cycle; drivers do these at attach time |
| A device interrupt | the kernel's level 2/6 handler → openpci's bridge server → the driver's handler: two register-glue crossings, the same dispatch openpci does on AmigaOS |
| A1200 Mediator outside the current window | one MMU fault per bank change, a cost of the hardware's small window, as on AmigaOS |

## 5. The interface for PCI drivers

```c
struct opci_dev *opci_find(prev, vendor, device);     /* OPCI_ANY wildcards */
struct opci_dev *opci_findclass(prev, class);
unsigned long    opci_cfgread(dev, reg, width);        /* 1, 2, 4 */
void             opci_cfgwrite(dev, reg, width, value);
unsigned long    opci_attr(dev, tag);                  /* OPCI_BARADDR(n), OPCI_INTLINE ... */
int              opci_obtain(dev); void opci_release(dev);
int              opci_master(dev);
char            *opci_region(dev, pciaddr, size);      /* legacy VGA and the like */
char            *opci_dmaalloc(dev, size, flags);
unsigned long    opci_busaddr(dev, va);                /* as the device sees it */
int              opci_intr(dev, struct opci_intr *);   /* oi_fn(oi_arg) nonzero = mine */
```

Each call is one LVO of openpci under amilib's lock, so all of them except the two config calls are for process context. Config reads and writes also work from an interrupt handler, but return all ones or do nothing if the library is busy. Interrupt handlers run at the bridge's Zorro level, called by openpci's own bridge server. On load, opci lists the bus on the console.

Attributes are the prometheus.library tags openpci keeps, so a driver gets BAR addresses already as kernel addresses.

## 6. Kernel contract

amilib reaches the Amiga host only through weak hooks, read through volatile pointers so no compiler folds them away. A kernel without them still loads both modules, but opci finds no boards and fails with `ENXIO`.

| Hook | Contract |
|---|---|
| `plat_zorro(i, zb)` | board `i` of the autoconfig list: physical base, size, manufacturer, product, `er_Type`, `er_Flags`, serial, diag vector |
| `plat_iomap(pa, size)` / `plat_iounmap` | supervisor, cache-inhibited, serialized mapping of a board (Zorro III boards are large: a Prometheus is 512 MB) |
| `plat_vtop(va)` | physical address of a kernel address |
| `plat_pagesize()` | MMU page size (absent: 4096) |
| `plat_zintr(intnum, fn)` / `plat_zunintr` | call `fn(intnum)` from the level 2 (`INTB_PORTS`, 3) or level 6 (`INTB_EXTER`, 13) handler |
| `plat_winrange(&lo, &hi)` | a kernel VA range kept unmapped for mmu windows: in [16 MB, 2 GB), inside one 512 MB block; 16 MB-aligned and 64 MB or more |
| `plat_remap(va, pa, len, mode)` | map pages uncached and serialized, or unmap them; flush the ATC; any IPL, including inside a fault |
| `plat_faulthook(lo, hi, fn)` / `plat_unfaulthook` | a supervisor access fault in `[lo, hi]` calls `fn(va, len, write)`; 0 = repaired, retry the access |

From the kernel proper: `kmem_alloc`/`kmem_free`, `printf`, `sleep`/`wakeup`, `vn_open`/`vn_rdwr`, `dlm_cacheflush`. `delayus`, `hrestime` and `cputype` are used when present.

## 7. mmu.library and the A1200 window

### 7.1 Why an adapter

mmu.library owns the MMU on AmigaOS: through 68040.library and 68060.library it builds and loads the translation tables. In our kernel AMIX owns them, so loading the real library would mean two masters over one MMU. amilib's mmu.library owns no tables. It keeps what a library describes (contexts, ranges, properties, windows, hooks) and sends anything that has to reach the MMU to the kernel through §6's hooks. The kernel's mappings remain the authority.

### 7.2 What openpci asks of it

| Use | Bridges | Here |
|---|---|---|
| cache-inhibit the bridge windows and BARs (`SetPropertiesA`, `GetMapping`, `RebuildTreesA`, `SetPropertyList`) | all | accepted. `plat_iomap` already mapped the boards cache-inhibited and serialized |
| a supervisor call around its config probe (`WithoutMMU`) | all | the function runs in supervisor state with the MMU on; the probe catches its own faults |
| a virtual PCI window (V46 context windows, exception hooks) | Mediator A1200 | with the window hooks: done through `plat_remap`/`plat_faulthook`; without: version 43, and openpci maps the board physically |

### 7.3 How the window works

The Mediator A1200 shows only a small window of PCI memory at a time, chosen by a bank register. openpci makes the whole PCI space appear at once:

1. At init it reads the MMU mapping (`GetMapping` on the default and supervisor contexts) and takes the largest **blank** region above 16 MB that stays inside one 512 MB block. It places the PCI memory there: PCI addresses equal those 68K addresses. Here `GetMapping` reports `plat_winrange` as that region.
2. It cuts the region into slices the size of the hardware window. It creates three private contexts: one **invalid**, and **bank 0** and **bank 1**, which remap every slice to the Mediator's physical window. For each slice it creates a context window in each home context that can show any of the three, and binds the slice to it (`MAPP_WINDOW`).
3. It installs an exception hook and activates it. Every slice starts invalid.
4. When a driver touches a slice that isn't shown, the access faults. The hook switches the previous slice to the invalid context and this one to a bank context (`SetContextWindow`), sets the bank register through the board's own routine, and returns 0. The access is retried and reaches the card.

In the kernel the same steps become: `plat_faulthook` delivers the fault, amilib builds the ExceptionData and runs the hook, `SetContextWindow` turns into `plat_remap` calls for the supervisor context's slices (remapped ranges to the board's physical address, everything else unmapped), and the retried access goes through. Each bank change costs one fault, as on AmigaOS.

## 8. Method: what the facts rest on

| Fact | Source |
|---|---|
| openpci's LVOs and arguments | its own pragmas and includes |
| its needs from exec, expansion, utility, dos, timer.device, intuition | openpci 40.15's calls, grouped by library base, and the strings it carries |
| how it probes the Prometheus and reads config space | openpci's behaviour (address-invariant LE bytes behind the board, `Supervisor` + VBR bus-error catch) |
| how it parses `PCI-Configuration` and loads plugins | openpci's behaviour (AllocDosObject RDArgs, FGets, ReadArgs per command, LoadSeg of unknown commands from `LIBS:PCI`) |
| mmu.library LVOs up to -360 | `MMU_lib.fd` 41.1 (vbcc stubs) |
| the V46 context-window LVOs (-426 to -450), the tags, the hook node, the MappingNode, the ExceptionData fields, the dispatcher's calling convention | mmu.library 47.11, checked against how openpci calls each one |
| how openpci gets mmu.library | openpci (FindName in LibList at init) and `PCIInit` (only needed when openpci loads first) |

Names we gave: LVOs -438 (`BuildContextWindow`), -444 (`SetContextWindow`) and -450 (`LayoutContextWindow`, after the MMULib 46.11 release note). Where a property bit's name is uncertain, the code uses only the behaviour seen (remapped, window, blank) and does not interpret the others.

## 9. Licensing and distribution

openpci.library and mmu.library are the copyrighted work of Thomas Richter and the mmu.library development group. Nothing of either is in the repository or the images. openpci.library is a disk library (`LIBS:` on AmigaOS), not part of any ROM: the user installs it (and, optionally, `PCI-Configuration`) into `/etc/conf/pci` from the openpci archive. The directory is a tunable (`opci_confdir`), so it can point wherever the system keeps it. No ROM is involved: on the Amiga host, Kickstart stays the machine's own ROM, used in place as everywhere else. Our code implements interfaces (LVO numbers, structure offsets, tag values) needed to interoperate with the binary the user supplies. It contains none of the libraries' code.

## 10. Stages

| Stage | Content | Check |
|---|---|---|
| P1 (done) | amilib, opci, dos, mmu adapter with windows; modules and `mods.sh` | compiles for m68k against stand-in headers |
| P2 | Amiga host layer: `plat_zorro`, `plat_iomap`, `plat_vtop`, `plat_zintr`, DLM on `unix-040`/`unix-060` | `modload opci` on a Prometheus or Mediator Z4 lists the bus |
| P3 | first PCI driver module (an RTL8139 or a VGA card's framebuffer) on `<sys/opci.h>` | traffic or a picture |
| P4 | window hooks (`plat_winrange`, `plat_remap`, `plat_faulthook`) | a Mediator A1200 with a card past the first window |
| P5 | `/dev/pci` and an `lspci` for users | |

## 11. Open questions

1. The AMIX autoconfig list and its interrupt chains: what `plat_zorro` and `plat_zintr` wrap on the stock Amiga kernel.
2. Kernel VA space for Zorro III boards: a Prometheus is 512 MB, a Mediator 4000 window up to 512 MB. Mapping whole boards may need the host layer to map only what openpci touches.
3. DMA: only the G-REX lets PCI devices master into CPU RAM; elsewhere DMA memory comes from PCI-side memory through openpci's providers. A driver that needs bounce buffers needs them from `opci_dmaalloc`.
4. A fault in a window raised by an interrupt handler: `plat_remap` must work at that level. To be confirmed in the host layer.
5. Mediator Z4, G-REX and Firestorm have not run at all yet, even modelled.
