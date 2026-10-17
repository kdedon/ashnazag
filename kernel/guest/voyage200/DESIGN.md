# Voyage 200: design

AMS runs natively from the user's dump.  Nothing decodes its accesses: the
register pages are memory holding constants, and the routines that need a
live answer are patched in the loaded copy to call host services.

## Finding routines

AMS keeps its ROM-call table pointer at `$C8`; the host finds the same table
in the dump by structure (a count, then that many entry points, nearly all in
Flash) and takes exported routines by index.  Routines AMS does not export are
found by their code (an instruction sequence unique to them).  No build is
named, hashed or addressed.

A patch is `jmp` (or `jsr`) to a stub on the host page `TI_HOST`: one
`illegal; rts` per service.  The module takes that illegal instruction (vector
4) only at a stub address and runs the service in guest context.

    routine --jmp--> TI_HOST + 8n: illegal --> service n (module) --> rts

## Subsystems

| Subsystem | Today | Pattern |
|---|---|---|
| Register pages `$600000`, `$700000` | decoded model | one page of memory each; constants written at start (`$600000` bit 2: battery comparator high, `$60001A` ON up, `$60001B` no key); AMS's writes land and nothing acts on them |
| Open bus (`$C00000`, where AMS posts keys) | decoded, reads 0 | one page of memory; anything else past Flash faults |
| Battery | bit 2 forced on reads | constant; the three `move.b #n,$600000` stores get bit 2 in their immediate |
| Key scan (ROM call `$298`) | matrix from `TI_KEYS` | service: AMS's scan result (key position, modifiers) from the host's keys |
| Modifier read (`$299`), 10-row read | matrix | service: the rows from the host's keys |
| ON key | matrix bit, auto-int 6 | auto-int 6 on press; the scan reports ON |
| Idle halt (stub that stores `$600005`) | write decoded | service: sleep until an interrupt in the mask |
| Auto-int 1, 3, 5 | clock tick | unchanged: host timer, `TI_SIG` frames, module state |
| LCD | `$600010` model | constant `$4C00` |
| Flash write (two routines), erase, block move (all copied to RAM to run) | command protocol on `flash.img` | services on the running copy: a write stores AMS's words, an erase frees a 64 KB block (ones, AMS's empty); AMS keeps its own range checks |
| Flash size probe (3.10) | ID protocol | service: 4 MB |
| Archive | `flash.img` | host files in `~/Voyage200/<env>/archive` (below) |
| Link port | status model | constants: idle |
| Power off (trap #4) | halt via `$600005`, LCD left on | AMS's own; its LCD off and on stores (`$60001C`) call services that blank and restore the host screen; it halts through the patched idle routine until ON |

## Archive as host files

Each archived variable is one file in `archive/`, in TI's variable file
format (`.v2e`, `.v2p` ... by type), so TI's link tools read and write
them.  The dump is read-only media; nothing else is kept.

AMS keeps archived variables as entries in 64 KB archive blocks (block
status word `$FFFE`; entry: status `$FFE0` valid, `$FFF0` being written,
`$FFC0` deleted; folder and name, 8 bytes each; a word from the symbol;
length in words with bit 15; the variable as in RAM).  Its handles point
into these entries and its archive code reads them directly, so the
entries stay in the running copy's archive blocks:

- Start: the host frees the copy's variable blocks and lays each file
  out as an entry in the top blocks; AMS's own start-up scan registers
  them.  At the first start, the dump's archived variables become files
  first.
- Archive, unarchive, delete, garbage collect: AMS's archive code reaches
  the blocks only through the Flash write and erase routines, which are
  services (above).  Patching each archive routine instead would
  duplicate AMS's archive manager and still have to write its entry
  format, which AMS reads back.
- After those services settle, the host walks the entries and makes
  the files match: a new or changed valid entry is written; a file it
  wrote or loaded whose entry is gone is removed.  Files added while
  the environment runs are left for the next start.

The block AMS moves to `$210000` and 3.10's size probe stay services;
they touch no archive block.

## Left as is

OS install paths (erase of the boot block, OS invalidate) are not patched;
reaching one is a hardware-page fault and ends the environment.
