# lisaguest design

The Lisa environment (`startlisa`, module `lisaguest`) boots the user's own Lisa ROM
and installed Lisa Office System natively on the host 68k. No chip is modelled and no
instruction is decoded: every routine that touches Lisa hardware is found by structure
in the loaded code and patched to call a host service. The user's files are never
changed.

## 1. Parts

| Part | Where |
|---|---|
| Segment MMU: SLR/SOR per context, RAM, stack and I/O segment types | module: host page mappings |
| Bus errors on invalid segments, 68000 frames | module, guestcore (`GPF_FRAME68K`) |
| Interrupts: retrace and the 20 ms timer, input | module: host timer, vIPL |
| Screen: 720x364 1 bpp in Lisa RAM, page from the video host call | launcher's display child, display service; centred, and scaled down on a smaller screen (columns or rows that meet are ORed, so 1-pixel black strokes stay) |
| Keyboard and mouse | display service input, queued in the module |
| ProFile, parameter memory, clock, serial number | launcher host calls |

The environment's folder (`$NATIVEHOST_ROOT`) holds the user's files:

| File | Use |
|---|---|
| `lisa.rom` | the user's ROM, 16 KB, read only (`-r` names another) |
| `profile.dc42` | the user's ProFile image (DC42 or raw 532-byte blocks), read only (`-d`) |
| `profile.writes` | blocks the Lisa wrote: block number, then 20 tag and 512 data bytes each |
| `parameter-memory` | the 64 parameter memory bytes (`-p`); the OS seeds it on first boot |

## 2. Boot

The launcher maps 2 MB of Lisa RAM and a copy of the ROM at $FE0000, patches the copy,
and starts it at its reset vector in supervisor mode. Patched ROM routines:

| Routine | Found by | Becomes |
|---|---|---|
| self-test | the reset vector | host call: RAM from $8000 to the end, logical 0 at its start ($2A4, $294, $2A8), the screen in its last 32 KB ($110), boot device ProFile ($1B3), D7's "device given" flag; then a jump to the boot device choice |
| boot device choice | its test of D7 bit 28 and load of $1B3, back to the `clr.l $180` that starts it | runs unpatched: it takes the given device and starts the ProFile boot |
| block read | jump table entry $90 | host call: D1 block, A1 tags, A2 data, carry on error |
| keyboard controller send | jump table entry $A8, which loads the controller's VIA | `rts` |
| keyboard controller reset pulse | its VIA load and bit clear, and the bit set after it | `rts` |
| video latch store | the store to $FCE800 | host call, D0 the page |
| disk controller command | its load of $FCC001 and command store | `rts` |
| boot menu input loop | interrupts masked, a call, then the clear of its key word | host call that stops the environment |
| restart | jump table entry $80 | host call that stops the environment |
| `bra .` | the word $60FE | a short branch to the stop call |

The ROM then reads block 0, checks the boot tag, sets its exception vectors and jumps to
the loader at $20000. Until then every vector points at the stop call. A ROM whose reset
entry, boot device choice or block read entry is not found is refused at start.

The loader reads the volume catalog through the block read, loads `SYSTEM.LLD`,
programs the segment MMU in setup mode through a routine it copies, and starts
`SYSTEM.OS`. The OS loads its code segments on demand and drivers (`SYSTEM.CD_*`)
from the volume.

## 3. CPU-side rules

- 68000 frames: exception frames are 6 bytes and a bus error adds the access word, the
  address and the instruction word; `rte` pops 6. The OS restarts a bus error by the
  frame's instruction word: a fetch fault carries `rte`'s (restart at the address), a
  data fault the instruction's own with pc + 2. This is how code segments load on demand.
- 24-bit addresses: the OS keeps flags in pointers' top byte. A fetch above 16 MB
  continues at the 24-bit pc, so a jump into a segment not yet loaded gives the OS its
  own fault. A data access above 16 MB maps that segment's pages at its 128 KB window
  there too, until the segment changes; a user context's window stays across a switch
  into context 0 where context 0 has no RAM. A window page the launcher already holds
  is left to the launcher and reported once; a guest access to a launcher page stops
  the environment with its address.
- Whole host pages: the host maps 4 KB, the Lisa MMU 512-byte pages. The loader's
  allocation rounding, the OS memory manager's segment sizes and the drivers' code space
  sizes are patched to 4 KB, so every segment starts and ends on a host page. The memory
  manager's region list stays exact: a new segment keeps its has-memory check, a resize
  whose rounded size is unchanged frees nothing, a moved segment's free region is what is
  left of the hole. A RAM page that still cannot be mapped
  stops the environment. A write to a read-only segment is a bus error, as on the Lisa.
- Contexts: context 0 in supervisor mode, SEG1/SEG2's context in user mode. guestcore
  calls the module on every virtual S change; the module remaps only segments whose
  SLR/SOR differ.
- User-mode A-lines (Lisa Pascal inter-segment calls) enter the dispatcher's user-mode
  tail through `gp_ualine` without a supervisor switch; the launcher registers the
  entry once vector 10 has the dispatcher's shape.
- Host calls: a patch is a `jsr` into the launcher, which runs in the same process above
  16 MB. The entry saves the registers, moves to a host stack, and returns with the
  registers and flags the service sets. Services reach guest memory only through the
  module, and the module holds interrupts while a host call runs. The launcher's
  buffers are allocated before the guest starts.
- Patching loaded code: blocks are patched as they are read, from patches worked out on
  the whole file at its first block (tag byte 11 keeps the block checksum); the loader's
  code is patched in RAM before each read; packed OS segments are patched by a host
  call at the end of the unpacker.

## 4. Hardware by patched routine (after the ROM)

| Access | Routine | Host service |
|---|---|---|
| ProFile VIA | `SYSTEM.CD_PROFILE`: step loop, drive check, completion | block read and write; the driver's own interleave table |
| setup mode, SLR/SOR, SEG1/SEG2 | loader's trap 6 handler and segment setup; OS SetContext | the segment table to the module |
| parameter memory | OS read and write | `parameter-memory` |
| serial number | loader's reader | constant |
| retrace, timer, interrupt status | LLD level 1 handler and its enable; the poll that runs them with interrupts masked | module tick; sources by host call |
| video latch | LLD setter | display page |
| keyboard VIA | COPS send; level 2 handler (the one ending in `rte`) and byte decoder | input queue: key bytes, mouse deltas, the power switch |
| power off | LLD power-off, now and at a time | host call that ends the environment |
| clock | LLD clock read | host local time |
| contrast, speaker volume | LLD setters | skipped; the volume reads 4 |
| speaker tone | LLD tone start and stop | silent; beeps keep their length |
| SCC | OS channel setup | returns at once |
| ProFile port interrupt enable | `SYSTEM.CD_PROFILE` shutdown, after the OS closed the volume | host call that ends the environment |
| VIAs and disk controller ID at start | loader and LLD start-up | branched over; the 2/5's defaults kept |
| drive type | OS probe | host call: a Lisa 2/5 |
| expansion slots | OS slot probe | no card |
| disk in place | micro diskette driver's status and I/O check | host call: no disk |
| diskette controller | micro diskette driver's command routine | host call: no result, no error |

No access reaches the I/O pages, at boot or later; `ls_hwfault` counts any that does
and each stops the environment. t_lisa asserts it stays 0, and that no user A-line goes
through supervisor mode in steady state.

## 5. Storage

The environment's folder holds the Lisa's files, one host file each (its bytes up to the
logical end), and per file a hidden sidecar `.NAME`: s-file number, 64-byte catalog
entry, header page (name, extents, the document's label). `.boot` holds the boot block
and the loader's blocks; `.volume` the volume's name, size and MDDF.

The OS's storage layer is patched by code signature to host calls:

| OS routine | answered from |
|---|---|
| s-list entry | sidecar's header page (a virtual page number) and extents |
| bytes of a page (header, data) | sidecar header; host file through the extents |
| s-file data | host file through the extents |
| catalog: search, entry pointer, next, path lookup, insert, delete, listing | sidecars' entries in key order |
| MDDF read/write | `.volume` |
| free bitmap | the union of all files' extents |

The loader reads the MDDF through the ROM's block read: it gets `.volume`'s copy, the tag's
checksum byte set to match.

Ended from outside (SIGTERM, SIGINT, SIGHUP; the display's Cmd-Q or a closed session), the
launcher presses the power switch: the OS frees the files it deleted, writes the MDDF and shuts
the ProFile driver down, which ends the process. Without that in 120 s it exits and says the volume was not closed.

Catalog callers get entries in the OS's own cache buffers: the entry at the data's start
and its place in the node's slot table, as the module's code reads a node. A buffer
given back dirty writes its entry to the sidecar and cuts the host file to the logical
end. New s-files make host files when their header is written; a renamed header renames
the host file; a deleted s-list entry removes file and sidecar. The ProFile driver is
not reached for the boot volume. Only the boot volume (the first one mounted) is the
folder; there are no directories. Calls for other devices run the OS's own routine: its
first instructions, copied to launcher data when patched, then a jump back past the patch.

## 6. Open

- Serial, printer and diskette block I/O have no host service yet.
- Keyboard tables exist for ADB hosts only.
- A screen narrower than 720 gets the screen scaled. The Office System's loader takes
  608x431 square pixels when the ROM's version byte ($FE3FFC) is 3 (3A, the Macintosh
  XL screen kit); the user's H ROM gets 720x364. Using that mode on an H ROM would mean
  reporting a 3A ROM to the loader.
- Speaker tones are silent; no host sound service yet.
