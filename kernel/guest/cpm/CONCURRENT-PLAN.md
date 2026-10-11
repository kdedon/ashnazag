# Concurrent CP/M-68K: reusing the C900 CP/M-8000 work

## 1. BDOS
Already ported from the C900 (`CPM3.md`): console and line editor, SCB (49), RSX chain (60), error modes (45), chain (47), parse (152), 108-112. Date stamps come from host atime/mtime.

Missing or stubbed:

| Feature | C900 | Here | Plan |
|---|---|---|---|
| Record locks 42/43 | `reclock.c` lock table | no-op | `fcntl` byte locks (record x 128), so they work across Unix processes. Keep the C900 return codes (8 locked, 14 table full). |
| CCP state 150/151 | in the BDOS | none | Needed by the C900 CCP. |
| XDOS 128-149 | `xdos.c`, `proc.c` | 141 only | See 3. |
| User-0 SYS fallback | yes | no | Port it (`v3ret` tests it). |
| GENCOM RSXes at load | `rsx.c` | fn 60 only | Port the load-time attach. |
| Passwords, 100, 104, 106 | on disk | Unix modes, ignored | Keep as is: Unix ownership is the protection. |
| f1'-f4' | kept | dropped | Leave dropped unless a program needs them. |

Shared source: `shim/*.c` is already a copy that differs from the C900's only in its header. Make one tree the owner and have the other import it: either a `cpmcommon/` directory synced by script, with a build check that fails on a diff, or a git subtree. The importable files are `con3.c` (conbdos), `scb.c`, `parsefn.c`, `rsx.c`, the CCP, and `shim/`. File I/O stays separate: host files here, the disk layer on the C900.

## 2. CCP
Yes. The C900 CCP is DRI's CP/M-68K CCP (C), extended to CP/M 3 level: TYPE/RENAME/ERASE prompts, SUBMIT and chaining, program return codes, DIRS, and optional search paths, named directories and IF/ELSE (`CCP.CFG`). Built with `-DCCPTRANSIENT`, `ccp.c` and `ccpext.c` cross-compile for 68k with our gcc and no errors. The only unresolved symbols are `bdos`, `__LOAD` and `strcmp`.

What it needs:
- A 68k `ccpcrt.s` (entry, `trap #2` gate, `__LOAD`), about 40 lines.
- BDOS 150/151 to keep its state page across warm boots.
- The warm-boot loader: run the program the CCP queued, otherwise reload `CCP.68K`. This is `ccprun.c` reduced to flat memory, since there are no segments.
- The command search must take `.68K` where the C900 takes `.Z8K`. `.COM` and `.CMD` stay as they are now (the BDOS shows them as `.68K`).
- An ELF to `.68K` converter. `tests/tos/elf2prg.py` handles the same 0x601A header; only the relocation format differs. It also lets the C900 utilities (`src/cmd`: SDIR, SET, SHOW, DATE, GET/PUT, SUBMIT) and tests be built.

DRI's 1.3 CCP stays in the build as a fallback (`startcpm -C dri`) until the new one passes `GROUP=cpm`.

## 3. Concurrency
The C900 scheduler (`proc.c`) is built around swapping 64 KB Z8000 segment pages and parking BDOS frames mid-call, so little of it carries over. Doing the scheduling inside the environment would also need a timer exception in the guest, context switches in our BDOS, and non-blocking host I/O everywhere, because one blocked `read` would stop every process.

**Recommended: each CP/M process is a Unix process.**
- Memory: Concurrent CP/M programs assume one address space. Queue messages and process descriptors carry pointers, and Create Process (144) starts code that lives in the caller's memory. So the environment's memory (raised from 4 MB to e.g. 16 MB) is mapped `MAP_SHARED` at 0 in every member: `envmem_region` already has the flag, and guestcore already handles guest fork. Each loaded program gets a partition, as on real Concurrent CP/M. The isolation boundary is between environments and from Unix, as it was on the real machine.
- Create Process 144 forks. Program load (59 or the CLI) allocates a partition in the shared table. Terminate (143) exits the Unix process, and the death hook frees its partition, its locks and its consoles.
- Shared table: a small `MAP_SHARED` file in M: holding processes, partitions, flag owners, queue names, console owners and per-console settings (width, page mode). One SysV semaphore guards it.
- Flags 132/133: one SysV semaphore each. Set when already set returns the overrun error.
- Queues 134-140: SysV message queues, keyed by environment and queue name. Conditional read and write use `IPC_NOWAIT`.
- Poll 131, delay 141, dispatch 142: `poll` or `sched_yield`. Priority 145 maps to `nice`.
- Per-process state (default drive and user, DMA, SCB, error mode) is already per process: it is startcpm's static data.
- Consoles: each console is a Unix tty. `startcpm -c N` joins a running environment as console N in a new terminal or session window, and xsession can open several at once. Attach, detach and assign (146-149) set the console owner in the table. A non-owner blocks on that console's semaphore. `envlock` changes from one session to one owner plus joiners.

## 4. Effort, order, risks
1. ELF to `.68K` tool, and build the C900 tests and utilities for 68k (S).
2. BDOS gaps: 150/151, user-0 fallback, GENCOM RSX, `fcntl` locks (S-M).
3. C900 CCP as `CCP.68K`, with DRI's CCP as the fallback. Run `GROUP=cpm` (M).
4. Shared memory, process table, fork, 143/144 (M).
5. Flags, queues, consoles, `startcpm -c` (M).
6. Shared-source import and its build check (S).

About 3-4 agent days in total.

Tests that carry over (C, rebuilt for 68k): `v3ret`, `scbtest`, `stampt`, `wildt`, `trunct`, `u0t`, `rsxt`/`rsxt2`, `errtest`, `cpm3fn`, `ccpct`, `lockt`, `xdosm`, `xdosd`/`xdose`, `xdospol`, `concd`/`conce`, `conch`, and `concb`-`concz` where they test the API rather than Z8000 paging. Drop: `split*`, `sysmode`, `sstkt`, `zdect`/`zrunt`, `idect`/`irunt` (we have `zdiff`/`idiff`), and the cost tests.

Risks:
- Real Concurrent CP/M-68K binaries and their exact XDOS numbering are untested here. The C900 uses MP/M-II numbers, and its 145 is a process count, not MP/M's Set Priority.
- A pointer into another partition only works if all members map memory at the same address, which this design guarantees.
- Fork cost per Create Process on a 68030.
- A user-supplied DRI CCP/M-68K, if found, would run under the same tables.

## Decisions (2026-10-10)
1. Unix processes over shared environment memory at 0, 16 MB (`-m`).
2. The C900 CCP is the CCP; DRI's is gone.
3. The C900 sources are copied here and owned here, GPL-2.0; nothing is kept in sync.
4. 145 is MP/M's Set Priority.
5. Consoles are separate terminals: `startcpm -c N`.
6. Record locks over host `fcntl` locks; passwords stay Unix modes.

## Status
Done: steps 1-5 (see `CPM.md`). Left: GENCOM's load-time RSX attach and the RSX tests (`rsxt`, `rsxt2`); 148 to another console; xsession opening extra consoles. Not ported (C900 scheduler internals and timings): `concl`, `concm`, `conco`, `concr`, `concs`, `concv`, `concz`, `xdospol`.
