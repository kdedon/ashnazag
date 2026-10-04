# ASLM libraries and the virtual Ethernet port

Open Transport pass-through: the `mkaslm` library builder and the `.ENET` driver with its kernel stations.

## Files

| File | Role |
|---|---|
| `mkaslm/mkaslm.c` | the tool: describe (`-d`), round trip (`-t`), layout check (`-l`), run (`-x`, 68k hosts, `mkaslmx.c`), build (`-o`) from an m68k ELF relocatable. K&R C, byte offsets; builds with the host cc and AMIX gcc |
| `mkaslm/aslmrt.s` | library runtime: the entry the manager calls, export callbacks, Main's first function |
| `mkaslm/test/auxtest.c` | test library: initialised pointers, a function table, calls across segments |
| `enet/enet.s` | `.ENET` DRVR over `/dev/otbstation`, position independent (about 1 KB) |
| `otbridge.diff` | kernel: stations in `otbridge`, SONIC virtual stations and local switch, A/UX ioctl pass-through, `/dev/otbstation` |
| `build.sh` | mkaslm (host, AMIX), `aslmrt.o`, `enet.drvr` |
| `test/host.sh` | host tests (below) |
| `test/guest.sh`, `test/t_otbst.c`, `test/t_aslm.c` | QEMU q800 tests on a tree with the diff applied |

mkaslm's own data format (inside `%A5Init`, after `__aslm_blk`): `AXL1`, below-A5 size, image length, pointer count, set count, per set (record, descriptor), pointer offsets, image; the runtime copies it below A5, zeroes the rest and adds A5 to the listed pointers.

Building a library:

```sh
m68k-cbm-sysv4-gcc -O -m68020 -fcall-used-d2 -c mod.c
m68k-elf-ld -r -d -o lib.o aslmrt.o mod.o
mkaslm -n 'AUXLib$x' -e 'OTModl$x,0x110,0x110,GetOTxInstallInfo,...' -o x.bin lib.o
```

Executable sections go to Main, `.a5init` to `%A5Init`, everything else (constants included, so data may point at them) below A5. Absolute references become segment or A5 relocations; references into another segment go through a jump table entry. PC-relative references across segments, 16/8-bit absolute ones and undefined symbols are errors. `-fcall-used-d2`: MPW code treats d2 as scratch. Pointer results are returned in d0 and a0 by this gcc, so MPW callers see them.

## Virtual Ethernet port

- Kernel (`otbridge.diff`): `/dev/otbstation` (major 55, minor 1, clone) gives a station with its own address in a SONIC CAM entry; frames between the host, stations and the wire are switched locally (details in the diff's `otbridge/NOTES.md` and `mac/sonic/NOTES.md`). Under the A/UX personality, `'o'` ioctls on major 55 pass through. Access: `/dev/otbstation` is 660 root:display (25), the group the Mac session needs for its screen; non-root users get one station each, with the derived address.
- Mac (`enet/enet.s`): a classic `.ENET` driver. Open opens the station (`O_NDELAY`) and takes the derived address; Control 245/247 multicast, 246 write (write data structure, source address filled in), 248/249 attach/detach protocol handlers (type 0: 802.3), 252 get info, 253 set general; 250/251 (`ERead`) `controlErr`. A VBL task reads frames each tick and calls the handler with a3 past a copy of the header, d1 bytes left, a4 ReadPacket, 2(a4) ReadRest.
- Unix calls are `trap #0` (same numbers under A/UX and AMIX); Toolbox traps and IODone go through the 8-byte glue slots after `AUXG`.

## Tests

- QEMU q800 (128 MB), network root, kernel with the diff (`test/guest.sh`): 77 pass, 0 fail.
  - `t_otbst` (49): who may open (group 25, one station per user); the driver, called with Device Manager registers, opens a station, reports its derived address, attaches handlers, sends ARP through its write data structure; the host's ARP reply (switched locally) and the user-network gateway's (over the SONIC and its CAM) reach the handler through ReadPacket/ReadRest with the documented register results; 802.3 and multicast delivery, detach, errors, queued completion through IODone, close. Kernel: attach rules, source enforcement, poll, SIGPOLL, station-to-station switching, statistics, address release, module unload.
  - `t_aslm`: mkaslm built for AMIX round-trips and checks its own library; `-x` loads the built library as the manager does (A5 world, JMP table, both relocation lists), runs entries 0–3 against a stand-in context and calls its exports by name with correct results; the 32-bit build refuses 75 malformed files without a crash.

## Known limits

- The Shared Library Manager loading a mkaslm library is untested (needs Mac OS 8.1 running).
- Meaning of the `libr` flag words, the manager's rule for duplicate class ids (design §7 q1), imports.
- The driver under Mac OS: no Slot Manager path yet (the fake card has no network sResource and `sGetDriver` is unsupported), so OT's NuBus scanner does not see it. Only the AMIX-native path is tested, not the A/UX-personality one (ioctl pass-through, `trap #0`).
- Real SONIC hardware (CAM with several unicast entries) is untested; only QEMU's model.

## Not yet done

- Network sResource with this driver in the synthetic declaration ROM and `sGetDriver` in the kernel Slot Manager.
- Receive by SIGIO (`OTB_STSIG`, `_AUXDispatch` 23) instead of the per-tick poll.
- mkaslm imports (records, `libi`, lazy stubs through the manager's binder).
