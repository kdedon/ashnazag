# amilib as a host for AmigaOS drivers

Status: design only. amilib runs one kind of binary today (openpci.library); this note lists what it needs to host AmigaOS drivers in general.

## What amilib is

A DLM module (`kernel/amiga/amilib`) that loads an AmigaOS hunk file into kernel memory and calls it. A shim answers the calls the binary makes into exec, expansion, utility, dos (read-only, one directory), mmu and timer.device. No AmigaOS runs.

```
 AMIX driver module (opci, ...)          AmigaOS binary (openpci.library)
   am_call(fn, regs) ------------------->  LVO function
                                           calls exec/expansion/... LVOs
   am_dispatch <-- am_entry <-- jsr stub <-'
   (handler in C on saved d0-a6)
 platform part (amx_*): memory, spl, delay, time, boards, mappings, interrupts
```

- AmigaOS structures keep their AmigaOS offsets; C reaches them by byte offset (`AB`/`AW`/`AL`), so compiler alignment doesn't matter.
- `amx_*` (amxplat.c) is the only code that knows the host kernel.
- Users count: `am_init`/`am_fini`; the module refuses to unload while held.

## Driver classes worth hosting

| Class | AmigaOS interface | AMIX side | Gain |
|---|---|---|---|
| SANA-II network | `.device`, SANA-II commands | DLPI interface for AMIX TCP/IP | every Amiga NIC with a driver (A2065, Ariadne, X-Surf, Hydra, PCI cards via openpci) |
| Picasso96 cards | `.card`, board-driver calls | display-service adapter | RTG cards with no AMIX driver (Picasso IV, CyberVision 64, ...) |
| AHI audio | `.audio` driver | host sound service | Zorro and PCI sound cards |
| Storage | `scsi.device`-style, HD_SCSICMD | AMIX disk driver | A4091, Fastlane, accelerator SCSI |
| Bridge libraries | vendor PCI libraries | `opci`-like module | setups openpci doesn't cover |

Order if started: SANA-II first (one module covers many cards), then P96 cards, AHI, storage.

## What is missing today

1. **Tasks and blocking.** amilib has one context: `Wait` and `WaitPort` never block, `FindTask` returns a single task. Devices create their own task (`AddTask`/`CreateNewProc`) and sleep on signals. Needed: exec tasks as kernel threads, with `Signal`/`Wait`/`SetSignal` mapped to sleep and wakeup, and `Forbid`/`Disable` as a lock and spl.
2. **Devices.** `OpenDevice` serves only the built-in timer.device. Needed: load a driver's `.device` file, run its `Init`/`Open`/`BeginIO`/`AbortIO`/`Close`/`Expunge`, and complete requests through `ReplyMsg` to ports owned by our wrapper (`am_iodone` exists for this).
3. **Interrupts per board.** `AddIntServer` chains run on the host's level 2/6 dispatch now; drivers also use `SetIntVector`, software interrupts (`Cause`) and PCI interrupts through openpci. Needed: all of these, running at the host interrupt level with the driver's data pointer.
4. **Memory types.** `AllocMem` with `MEMF_CHIP`, `MEMF_24BITDMA` and physically contiguous DMA buffers; the host must hand out memory the board can reach and never move it.
5. **More libraries.** Typical drivers open `utility.library` (done), `dos.library` for config files (read-only now), `intuition.library` for requesters (answer "cancel"), `prometheus.library`/`pci.library`, `cia.resource` timers, and `misc.resource`.
6. **Per-class wrappers.** One AMIX module per class translating between the AmigaOS interface and the host: SANA-II ↔ DLPI (buffer-management hooks copy to and from mblks), P96 board calls ↔ display-service session ops, AHI ↔ sound service, HD_SCSICMD ↔ the AMIX disk interface.
7. **Configuration.** Where each driver's settings come from (tool types, `ENV:` variables): a per-driver file under `/etc/conf/amiga/`.

## Rules

- Driver binaries are the user's (commercial or freeware), installed like ROMs; never shipped.
- A driver runs in kernel context: a crash there is a kernel fault. Each wrapper validates what crosses into the host, and the module is optional.
- Prefer a native AMIX driver where one exists (check amigaux.org first); amilib covers the rest.
