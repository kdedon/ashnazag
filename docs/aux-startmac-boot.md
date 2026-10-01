# A/UX `startmac` boot sequence (32-bit)

This is static analysis of `/mac/bin/startmac`, `/shlib/libmac1_s` and `/mac/lib/Patches/Patch.067C` from A/UX 3.1. Addresses are link addresses. Disassembly came from `tools/coffdump.py`. "Unconfirmed" marks things the code does not prove.

## Key finding

The Mac environment brings up the Toolbox through the ROM's table of entry points for other operating systems (the "Foreign OS" table). It builds a table of ROM entry points from ROM header offset `$16`, calls the ROM's **trap-table init (Foreign OS entry 0) in user mode**, and replaces almost everything else with its own code in `Patch.067C`. The kernel supplies hardware-derived low-memory values through `/dev/uinter0`. `hwCbAUX` is set by `Patch.067C` itself.

## `startInfo` structure (`__tb_startInfo`, pointer at `0x47ec2ce0`)

| Off | Meaning | Set by |
|---|---|---|
| `+0x0c` | exit function | `__tb_BuildMachine` |
| `+0x10` | max open files: `-F` stores here, but `__tb_BuildMachine` then **unconditionally** sets `0x28` (40) | `__tb_BuildMachine` |
| `+0x14` | event queue size: `-e` stores here, but `__tb_BuildMachine` then **unconditionally** sets `0x20` (32) | `__tb_BuildMachine` |
| `+0x18` | Mac memory size: `-m`, else `__tb_GetVMSize`, else physmem from `uvar()`. Capped at 16 MB, minimum 3 MB | `__tb_SetDefaults` (bt_3) |
| `+0x1c` | home directory | bt_3 |
| `+0x20` | System Folder path (`__tb_FindSystemFolder`) | bt_3 |
| `+0x24` | System file name: `-S`, else `"System"`, or `"System.back"` if flag `{9:1}` is set | bt_3 |
| `+0x28` | Finder name: `-f`, else `"Finder"` | bt_3 |
| `+0x2c` | debugger name: `-d`, else `"MacsBug"` | bt_3 |
| `+0x30` | patch file path: `-P` or env `TBPATCHES`, else built by `getPatches` | options / `getPatches` |
| `+0x34` | console fd (`/dev/console`, else dup of fd 0 if it is the same char device) | `openDevices` |
| `+0x38` | `/dev/uinter0` fd | `openDevices` |
| `+0x3c` | layer id from uinter ioctl 21 | `__tb_BuildMachine` |
| `+0x40` | option flag bits, see [Flags](#flags-si0x40) | `__tb_FetchEnvironment`, `__tb_ParseArguments`, `__tb_SetOption` |
| `+0x42` bit 7 (`{0:1}` of the byte at +0x42) | `minimal` option | `__tb_SetOption` |
| `+0x44` | screens[6], 8 bytes each: `{u8 slot, u8 slotId, u8 extDev, u8 pad, u32 devBase}`; `slot == 0xff` means unused | uinter ioctl 34 |
| `+0x74` | ROMBase (`0x40800000`; 24-bit: `0x800000`) | `getROMImage` |
| `+0x78` | ROM size (`getROMSize`: `0x40000` if version `0x178`, else ROM header `+0x40`) | `getROMImage` |
| `+0x7c` | ROM version word (`ROM+8`) | `getROMImage` |

The whole `startInfo` struct is in `libmac1_s` `.bss` (pointer `__tb_startInfo` → `0x47ec3348`), so **every field starts at 0**. Defaults come only from the code below.

### Command-line options (`__tb_ParseArguments`, getopt `u:s:f:d:F:e:o:m:P:S:l:6`)

| Option | Effect |
|---|---|
| `-u user` | home directory of *user* → `+0x1c` ("No Such User" and exit 3 if unknown) |
| `-s dir` | System Folder → `+0x20` |
| `-S name` | System file name → `+0x24` |
| `-f name` | Finder name → `+0x28` |
| `-d name` | debugger name → `+0x2c` |
| `-P path` | patch file → `+0x30` |
| `-F n` | → `+0x10` (overwritten later, see above) |
| `-e n` | → `+0x14` (overwritten later) |
| `-m size` | memory size → `+0x18` (`__tb_ScaleArg`: number with optional `K`/`M` suffix) |
| `-o name[=n]` | `__tb_SetOption(name, n)`, see below |
| `-l` | flag `{12:1}` (no consumer found outside `__tb_DumpStartInfo`; meaning unknown) |
| `-6` | flag `{15:1}` (same bit as `oldsystem`) |

### Environment variables (`__tb_FetchEnvironment`)

- Table at `0x47ec24e8`, pairs `{env var, option}`, each passed to `__tb_SetOption`: `TBVERBOSE`→`verbose`, `TBOPEN`→`open`, `TBWARN`→`warn`, `TBPARANOID`→`paranoid`, `TBTRAP`→`traps`, `TBDEBUG`→`debug`, `TBRAM`→`ram`, `TBFMDEBUG`→`fmdebug`, `TBREADONLY`→`readonly`, `TBOPENDLOG`→`opendlog`, `TBOLDSYSTEM`→`oldsystem`, `TBMINIMAL`→`minimal`. The value is the option argument.
- `TBSYSTEM`: System Folder (`+0x20`) if it names a directory.
- `TBPATCHES`: patch file path (`+0x30`).
- `TBMEMORY`: memory size (`+0x18`); otherwise the `aux_get_pref("memory size")` preference.
- The program's own `noEvents` global is copied into flag `{9:1}`. It is 0 in `startmac` (in `.bss`).
- `TBTRANSLATEUXONLY`, `TBFMIGNORECASE` and `TBTRANSLATESLASHES` (set by `mac32`) are not read by `libmac1_s` startup; they are File Manager options, probably read by `Patch.067C`.

### Flags (`si+0x40`)

Bit-field offsets as the code uses them (`{0:8}` = the byte at +0x40, `{8:1}` = MSB of the byte at +0x41, and so on).

| Field | Set by | Effect |
|---|---|---|
| `{0:8}` mask `0x01` | `warn` | `startmac` calls `__tb_DumpStartInfo` if set; also enables warnings |
| mask `0x02` | `open` | debug output (not traced) |
| mask `0x04` | `paranoid` | debug checks (not traced) |
| mask `0x08` | `traps` | `doDispatch`: `installAline(DebAlineHandler)` |
| mask `0x10` | `verbose` | debug output |
| mask `0x20` | `fmdebug` | File Manager debug |
| whole byte | `debug=N` | sets the byte to *N* |
| `{8:1}` | `ram` / `TBRAM` | **copy the ROM into RAM** (see ROM mapping). Default 0 |
| `{9:1}` | program `noEvents` | use `System.back`; `InitEventMgr` skips event setup (for non-interactive COFF tools) |
| `{10:1}` | (no setter found) | `terminate` calls `abort()` (core dump) on fatal error |
| `{11:1}` | (no setter found) | only printed by `__tb_DumpStartInfo` |
| `{12:1}` | `-l` | only printed by `__tb_DumpStartInfo` |
| `{13:1}` | `readonly` | tested in 5 `Patch.067C` File Manager routines; read-only behaviour inferred from the name |
| `{14:1}` | `opendlog` | `Patch.067C` skips installing a `$A9C9` (`SysError`) replacement |
| `{15:1}` | `oldsystem`, `-6` | `startmac`: if the chosen System file's version is below `$700` and this flag is clear, it falls back to `/mac/sys/System Folder` (exact fallback logic not fully traced) |
| `+0x42 {0:1}` | `minimal` | minimal start-up in `Patch.067C` (several consumers; not traced) |

**Defaults: all flags 0.** So a plain `startmac` maps the ROM directly (no copy), loads `System`, and uses 40 files and a 32-entry event queue.

## Boot sequence

`startmac` is a COFF program linked against `libmac1_s` and `libc1_s`. `bt_N` are the `libmac1_s` branch-table targets; for example `bt_7` is the body of `__tb_BuildMachine`.

1. `_start` → `initfpu`, `__istart` → `startmac()` (0x10000228).
2. `startmac()`:
   1. `__tb_FetchEnvironment` (bt_1), `__tb_ParseArguments(&argc,&argv,&envp)` (bt_2); `__tb_SetDefaults` (bt_3) fills the startInfo defaults.
   2. `updateSysFolder(si)` (may run `/usr/lib/updtsysfldr`).
   3. `__tb_BuildMachine(1)` (bt_7, 0x47e05e94). See below. It exits if this returns NULL.
   4. `launchVars()` sets low-memory globals: `ReadDateTime($20C)`, `$8F2=$FF`, `$A1C=0`, `$BC2=0`, `$BC6=$FFFF`, **`CurrentA5 ($904) = 0x100435a4`** (startmac's A5 world).
   5. If `si->flags` bit 0 is set: `__tb_DumpStartInfo`.
   6. `main()` is now Mac code: `ctop("MultiFinder")` (or argv[1]), `HSetVol(0, BootDrive $210, 0)`, then **`Launch`**, making Toolbox calls through `libmac1_s` A-trap glue. Then `exit`.
3. **`__tb_BuildMachine(mode)`**: mode 1 = create the environment (startmac); mode 0 = attach an existing one (COFF Mac apps: CommandShell, launch…); mode 2 = unconfirmed.
   1. `openDevices`: `open("/dev/uinter0", O_RDWR)` → `si+0x38`; `open("/dev/console")` → `si+0x34`. **ioctl Q0 returns the driver version, which must equal 5**, or startup aborts with "Toolbox version = 5, driver version = …".
   2. mode ≠ 0: ioctl **Q52** (`UI_test`) must succeed, else "The toolbox environment already exists". mode 0: ioctl **Q50** (`UI_sync`) must succeed, else "Can't attach to the toolbox environment".
   3. `openSegments` → `shm_attach('tLOW', &si->memsize, addr 0, create)`. See the memory map.
   4. mode ≠ 0 (create):
      - `memset(0, 0xFF, 0x4000)` fills low memory with `0xFF`.
      - `uip = 0x3000`; ioctl **Q8** (`UI_unmap`), then **Q7**`(0x3000)` (`UI_MAP`). See [The ui page](#the-ui-page-q7-ui_map).
      - ioctl **Q21** (`UI_CREATELAYER`) → layer id `si+0x3c`. On failure: Q8.
      - mode 2: `shmctl(id, IPC_RMID)`; otherwise ioctl **Q46** (`UI_SHMID`, passes the tLOW shm id).
      - ioctl **Q36** (`UI_ATTACHGFD`).
      - `__tb_remap_fd` on the uinter and console fds (moves them to high fd numbers).
      - `mapScreens`: ioctl **Q34**(`&si->screens`, 48 bytes). See [Screen mapping](#screen-mapping-q34-ui_phys_screens).
      - `getROMImage(si, 0)`: see the ROM section.
      - ioctl **Q1**(1) (`UI_SET`).
      - `getPatches(si, 1, 0)` **loads and runs `Patch.067C`**. Control enters Mac code here.
      - Success means low-memory `ApplZone ($2AA)` and `SysZone ($2A6)` are both non-zero; `__tb_BuildMachine` then returns `si`.
   5. mode 0 (attach, COFF Mac application):
      - `ui_setcoffname(coffbuf)` (ioctl **Q63**).
      - `ui_attach(0, sizeResource, flagsResource)` (ioctl **Q37**, 10-byte arg).
      - On `EACCES`: "This command cannot be used in 24-bit mode."
      - Then ioctl **Q1**(1).
      - COFF apps are loaded by `__tb_coff_allocate` into a private shm segment (`shmget(IPC_PRIVATE, text+data+bss, 01777)`) attached at the app's text address.

## ROM mapping (`getROMImage`, 0x47e05ae0)

- 32-bit: copy target `a3 = 0x40800000`, physical-map address `a5 = 0x50000000`. 24-bit: `0x800000` and `0xF00000`.
- `ioctl(uinter, Q5 UI_ROM, addr)`: the kernel maps the physical ROM at `addr`. `addr = a5` if flag `{8:1}` (`ram`) is set, else `a3`.
- **Default (flag clear): the physical ROM is mapped directly at `0x40800000`.** No copy.
- With `TBRAM` / `-o ram` (copy path):
  1. `shmget(IPC_PRIVATE, romsize, 01777)`
  2. `shmat(id, 0x40800000, 0)`, which must land exactly there
  3. `shmctl(IPC_RMID)`
  4. `memcpy(0x40800000, 0x50000000, romsize)`
  5. ioctl **Q6** (`UI_unrom`) removes the physical mapping

  This gives a writable ROM image (for debugging or patching).
- `si->ROMBase = 0x40800000` either way; `si+0x7c` = the word at ROM+8, read directly (no remapping).

## Patch selection and loading (`getPatches`, 0x47e05c3c)

- Path = `si+0x30` if set, else `sprintf("%s/Patch.%04.4X", si->sysFolder(+0x20), romVersion)`. If that file isn't a loadable COFF: `"/mac/lib/Patches/Patch.%04.4X"`. For ROM version `$067C` this gives `Patch.067C`.
- `__tb_coff_info` → `getinfo` fills a 0x1e-byte info block: `+0` a.out entry, `+4/+8` .text vaddr/size, `+0xc/+0x10` .data, `+0x14/+0x18` .bss, `+0x1c` "has relocations" flag (refuse to load if set).
- `__tb_coff_load`: **no relocation**. `.text` and `.data` are read from the file straight to their link addresses; `.bss` is zeroed in place. Then `CacheFlush` (`sysm68k` subcommand `0x69`).
- Low memory `MMU32Bit ($CB2) = 1` when the third argument is 0 (32-bit).
- Calls the entry `start(si, &coffinfo, environ)`.

`Patch.067C` a.out header: magic `0x107`, entry `0x7628` (`start`), text `0x4000`, size `0x4e6f0`; data `0x526f0`, size `0x79e0`; bss `0x5a0d0`, size `0x3446c` (ends `0x8e53c`). It lives **inside the tLOW segment** (Mac RAM), just above low memory.

## ROM versions accepted

The ROM version word is ROM+8. Both supplied ROMs (IIci, Quadra 700) report `$067C`; later ROMs report `$077D`. The Quadra 610/650/800 and Centris also take `Patch.067C`; A/UX 3.1 boots on a Quadra 800. A/UX 3.1 checks the word in these places:

| Where | Check | Effect |
|---|---|---|
| kernel `setupProductInfo` (0x589ee) | `RomVersion (0x5b6bc) = ROM+8`; `>= $067C` (unsigned) | "universal" ROM: copies ProductInfo from the ROM's `UnivInfoPtr` ($DD8) and relocates it |
| kernel `setup_lowmem_globals` | `>= $067C` | sets kernel low memory `$DD8` |
| kernel `getROMSize` (0x10086072) | `< $067C` → 256 KB, else ROM header `+$40` | ROM size for `UI_ROM` |
| kernel `initROMArray` (0x10046292) | `== $0178`: hard-coded ROM addresses (`$408064BA`, `$40804152`); `== $067C`: entry 3 replaced by the kernel's `romaline` value; **any other version: generic**, all entries from the Foreign OS table at ROM+$16 | kernel's ROM entry points |
| kernel `doPatches` | `== $067C` (+ machine ID 9 or 11) | picks a `secondaryInit` address; other ROMs: none |
| `libmac1_s` `getROMSize` | `== $0178` → 256 KB, else header `+$40` | user-side ROM size |
| `libmac1_s` `getPatches` | none; builds `Patch.%04.4X` from the version | patch file choice |

So the **kernel accepts any universal ROM (`>= $067C`) generically**. The user-side limit is the patch file:

- `getPatches` tries `<System Folder>/Patch.%04.4X`, then `/mac/lib/Patches/Patch.%04.4X`, then fails with `sys_error(path)` and `__tb_BuildMachine` returns NULL. There is no version remapping.
- This installation has only `Patch.0178` (Mac II/IIx/IIcx/SE/30 256 KB ROM) and `Patch.067C` (IIci/IIsi/IIfx and other 512 KB universal ROMs).
- A `$077D` ROM would fail at `getPatches` on this installation because `Patch.077D` doesn't exist.
- Forcing `Patch.067C` with `TBPATCHES` or `-P` is not expected to work: `Patch.067C` contains 17 hard-coded ROM addresses (`jsr $4081b708` etc. in `mGetResource`, `addromrsrc`, `unixchkload`, `GetFCBInfo_patch`) plus `$29A = ROMBase+$1D470`. `Patch.0178` has 55. **Patch files are built for one ROM version family.** The fixed addresses work across `$067C` ROMs because the later $067C ROMs are "overpatch" ROMs that keep the IIci ROM's layout. In the IIci and Quadra 700 images, 15 of the 16 fixed addresses hold identical code, and the 16th ($F04C) is overpatched with a `bsr.l` into the upper-half patch code.

Kernel machine support is separate: `boardinit` switches on `machineID` (0x5afa6, copied from the A/UX Startup boot parameters) and panics "unsupported motherboard type" for IDs outside its table (IDs 4–7, 9, 11, 16, 18, 20, 24, 28, 33, 34, 42, 43, 46, 50, 51, 57). `machineID` = Gestalt `'mach'` − 2 (Quadra 800 = 33, Gestalt 35), consistent with `boardinit`'s per-model CPU clocks ([mac-q800-hardware.md](mac-q800-hardware.md)) and from A/UX Startup's hand-off ([mac-boot-and-memory-map.md](mac-boot-and-memory-map.md)).

## `Patch.067C` `start` (0x7628)

In order:

1. `machinepid = getpid()`; save coffInfo, startInfo, environ.
2. **`doROMArray`**: `table = ROMBase + *(u32*)(ROMBase+0x16)` (the **Foreign OS table**); `ROMArray[i] = ROMBase + table[i]` for i = 0..4, i.e. entries 0 (trap-table init), 1 (A-line handler), 2, 3 (`StartSDeclMgr`), 4 (`InitMemVect`). Entry 5 and any later words are not read.
3. **`doVariables`**:
   - Fills `0..0x2FFF` with `0xFFFFFFFF`, then clears or sets many globals.
   - `ROMBase ($2AE) = si+0x74`; `SysResName ($AD8)` = si System name; `FinderName ($2E0)`; `MemTop ($108)`, `BufPtr ($10C)`, `RealMemTop`, `PhysMemTop` = `si->memsize`.
   - **`GetLowFromKernel`** copies kernel values with ioctl **Q32** (`UI_copy_out`), per the table at `0x52fa0`: `$28E` ROM85 (2), `$B22` HWCfgFlags (2), `$D00` (2), `$D02` (2), `$12F` CPUFlag (1), `$21E` KbdType (1), `$DD8` UnivInfoPtr (4), `$CB3` BoxFlag (1), `$CB1` MMUType (1). It records `realBoxFlag` and rewrites BoxFlag to `$10` for some models.
   - **`InitUniversalPtr`**: ioctl **Q67** (`UI_get_prodinfo`) into `prodinfo`; relocates its internal pointers; `UnivInfoPtr ($DD8) = &prodinfo`.
   - **`HWCfgFlags ($B22)`: `bset #1` on the high byte sets bit 9 = `hwCbAUX`; `andi.w #$7BFF` clears bit 15 (SCSI) and bit 10 (ADB).**
   - If BoxFlag ≥ 12: `$DD4 = $700`. Always: `$DD0 = $1000`.
   - Restores `MMU32Bit`; `uip = 0x3000`; `Lo3Bytes ($31A) = $FFFFFF`.
   - Hardware bases are set to Mac II values but point at nothing mapped: `VIA ($1D4) = $50F00000`, `$1D8`/`$1DC = $50F04000`, `SoundBase ($266) = $50F14000`.
   - `$29A = ROMBase + $1D470`; `$3FE0 = 0`.
4. **`doDispatch`**:
   - **calls `ROMArray[0]` = ROM trap-table init directly, in user mode**. It fills the OS table (`$400`) and Toolbox table (`$E00`) from the ROM dispatch table and installs the ROM's A-trap dispatcher (Foreign OS table entry 1) into vector `$28`, which is how vector `$28` gets set (not yet confirmed from the ROM's code).
   - Overlays the tables with `patchTable(0xE00, ToolPatch, 0x400, unimpl)` and `patchTable(0x400, OSPatch, 0x100, unimpl)`, where `unimpl = NGetTrapAddress($A89F)`.
   - If `si->flags` bit 3 is set: `installAline(DebAlineHandler)`.
5. `doAUXPrePatches`: trap-table overrides.
6. `doOSModules`: `InitMemMgr` (the patch's own 24/32-bit Memory Manager; `$322 = $8000`, `$130`/`$114` from MemTop), `InitDevMgr`, `init_FileManager`, `initScrap`, `_InitResources ($A995)` (opens the System file), `NMInit`, `InitExpandMem`, `InitGestalt`, `TEGlobalInit`.
7. `doLaunch`: `HSetVol` to the System Folder volume, `ReadDateTime`, resource checks ("The System file is missing, protected, or corrupted"), `InitAllPacks`, …
8. `doVideo` (ea94), `doToolboxModules`, `doCursor`, `startTime`, **`InitEventMgr`** (uinter Q9, Q23, Q24).
9. `InitExceptions(0)`:
   - Every vector `$08..$FC` still `0xFFFFFFFF` becomes `fault`, except `$80` (`trap #0`), `$88` (`trap #2`) and `$BC` (`trap #15`), which are left for the kernel. `$28` is already set by `ROMArray[0]`.
   - ioctl **Q68** (`UI_get_interr_vectors`) fills `$C0`, `$CC`, `$D0`, `$D4`, `$D8` (the FPU exception vectors).
10. `signal(3 SIGQUIT, entermacsbug)`.
11. `BootFromRsrc(boot_params, 0x8a)`: `GetResource('boot', …)` and runs the boot-block code. If that returns non-zero, it skips straight to the return.
12. `doStartupScreen`, `InitMacsBug` (`BufPtr -= $1C00`), `InitExceptions(2)`, `doMacPatches` (`GetResource` + `callPatch` for System patch resources), `doAUXPostPatches` (more trap overrides, `InstallMyGestalts`, `InitSndManager`, `InitFolderManagerPatch`, `InstallScriptPostPatches`), `InitReaper` (SIGCLD, 18), `doInits` (INITs), `setup_desktop`.
13. Returns `startInfo` to `getPatches` → `__tb_BuildMachine` → `startmac()` → `main()` → `Launch` Finder.

## Memory map of a 32-bit Mac process

| Range | What | Created by |
|---|---|---|
| `0x0` .. memsize (+4 MB if memsize > 8 MB) | **Mac RAM**: shm key `'tLOW'` (`0x744c4f57`), mode `01600` on create (`0600` on attach). Attached with `shmat(id, 1, SHM_RND)` so it lands at 0. Retried in 1 MB steps down to 3 MB, so SHMMAX and RLIMIT_VMEM silently cap it (`uinter` raises SHMMAX, the `startmac` wrapper the soft limit) | `openSegments` / `shm_attach` |
| `0x0`–`0x3FFF` (inside tLOW) | low memory, vectors, trap tables (`$400` OS, `$E00` Toolbox) | filled with `0xFF`, then `doVariables` |
| `0x3000`–`0x3FFF` | the **ui page**: the process's own tLOW page, locked and shared with the kernel by **Q7** (holds `struct ui_interface`) | `__tb_BuildMachine` |
| `0x4000`–`0x8e53c` | `Patch.067C` text/data/bss at link addresses (inside tLOW) | `__tb_coff_load` |
| above `0x8e53c` .. `MemTop` | System heap, application heap (set up by the patch's Memory Manager) | `InitMemMgr` |
| `0x10000000`– | `startmac` text (`0x100001e8`), data, bss | exec |
| `0x3FFF0000`–`0x3FFFFFFF` | the Process Manager's stack while it disposes of a process (System `scod` −16468 sets SP `0x3FFFFF00`, HeapEnd `0x3FFF0000` when SP is below `$1EF4`); zero-fill memory | `uinter` at Q5 |
| `0x40800000` + ROM size | physical ROM mapped directly (default), or a private shm copy (`TBRAM`) | `getROMImage` (Q5) |
| `0x47e00000`–`0x47ec4000` | `libmac1_s` text / data / bss | exec (shared lib) |
| `0x47f00000`– | `libc1_s` | exec (shared lib) |
| `0x50000000` | physical ROM (temporary, copy path only; unmapped by Q6) | Q5 |
| `devBase & 0xFF000000`, 16 MB per screen | frame buffers: the card's whole NuBus slot region mapped **identity** (VA = PA); built-in video: `0x50000` bytes | Q34 |
| Unix user stack | `startmac` / Mac code run on the normal process stack (A/UX stack top not determined); a 64 KB alternate signal stack is allocated from the Mac heap via `sigstack` in `install_handler` / `attach_timer` | kernel / `Patch.067C` |

## The ui page (Q7 `UI_MAP`)

Kernel `UI_map` (0x10087714), argument = user address (`0x3000`):

1. EINVAL if a ui page is already set (`ui_addr` non-zero).
2. `useracc(uaddr, 0x1000, 0x10)`: locks the 4 KB user page in memory (the same lock `undma` releases). No new memory is created: the page is the process's own tLOW page at `0x3000`.
3. `realvtop(uaddr, 1)` gives the kernel address of that page → `ui_addr`; the user address goes to `ui_uaddr`.
4. Initializes `struct ui_interface` (from `sys/uinter.h`) there: `c_mx`, `c_my`, `c_cx`, `c_cy` = 0 and `c_button` (+0x458) = the current `mouse_button`.

`struct ui_interface` (offsets computed from the header, `c_button` confirmed by the code):

| Off | Field | Meaning |
|---|---|---|
| 0x00 | `c_mx`, `c_my` | mouse position |
| 0x08 | `c_cx`, `c_cy` | cursor position |
| 0x10 | `c_smx`, `c_smy` | screen row size in pixels |
| 0x18 | `c_ssx`, `c_ssy` | visible screen size |
| 0x20 | `c_hpx`, `c_hpy` | cursor hot spot |
| 0x28 | `c_cursor` | cursor data pointer |
| 0x2c | `c_mask[16]` | cursor mask |
| 0x4c | `c_data` (union, 0x400) | saved pixels under the cursor, 1–32 bpp |
| 0x44c | `c_style` | cursor style (`CUR_SMALL1`..`CUR_SMALL32`) |
| 0x450 | `c_newcrsr` | |
| 0x454 | `c_lock` | cursor lock |
| 0x458 | `c_button` | mouse button |
| 0x45a | `c_modifiers` | modifier keys |
| 0x45c | `c_mlookup[10]` | mouse acceleration table |
| 0x470 | `c_keythres`, `c_keyrate` | auto-key threshold and rate (ticks) |

The kernel draws the cursor and tracks the mouse in this page from interrupt level; the Mac side reads and writes the same memory. `UI_UNMAP` (Q8) unlocks it.

## Screen mapping (Q34 `UI_PHYS_SCREENS`)

Argument: `struct screens` = 6 × `struct screen {char dCtlSlot; char dCtlSlotId; char dCtlExtDev; long dCtlDevBase;}` (8 bytes each, 48 total). Kernel `UI_phys_screens` (0x10086f78):

1. Needs a layer (`ui_get_layer`) and a screen table slot; EINVAL if screens are already mapped.
2. Sets every `screen[i].dCtlSlot = 0xFF` (unused) and `dCtlDevBase = 0`.
3. For each kernel video device `video_desc[i]` (i < 6) marked active (bit 0 of +0x2d):
   - `base = desc+0x14` (frame buffer address).
   - Normal slot card: map `0x1000000` bytes (16 MB) at `base & 0xFF000000`, **identity** (virtual = physical NuBus slot space). For a 24-bit process (`SMAC24`, proc flag 0x800) also record a 1 MB mapping at `addr2slot(base) << 20`.
   - Built-in video (global `0x5b3aa` set and `desc+0x1e == 0`): map `0x50000` bytes; `dCtlDevBase` is rounded down to 1 MB; the layer records `+0xea = 0x40000000`, `+0xee = 0x100000`.
   - Fills `screen[i] = {desc+0xb0 slot, desc+0xb1 sResource id, desc+0xba extDev, base}`.
4. `attach_screens` → `map_screen` for each (switching to the process's MMU mode with `swapMMUMode`) → `ui_phys(va, pa, len)`, which uses the same per-process physical-mapping table (u+0x3d2) and `dophys` region code as the SVR2 `phys()` system call. On failure it undoes the ROM (Q6) and screens (Q4).

Cache mode of these mappings: not determined (`dophys` region; the PTE attributes weren't traced).

## `startmac24` (cursory)

- Statically linked. Text at `0x780000`, data at `0x7c0000`, both below 8 MB so they fit in the 24-bit space. There is a `.low24` section.
- Its own `machine24.c` code (`do32bitRom`, `phys_24bit_ROM`) maps the ROM at `0x800000`.
- Same `Patch.%04.4X` lookup. It references a "System Folder24" string.
- Uses `libmac_s` rather than `libmac1_s` (unconfirmed).

## Open

- `ROMArray[1..4]` are stored but have no direct reference in `Patch.067C`; they may be used indirectly.
- `doVideo`, `InitEventMgr` and `startTime` internals (covered in part by `aux-interrupts-and-gateways.md`).
- Consumers of the `open`, `paranoid`, `minimal` flags and of `{11:1}`/`{12:1}`.
- Where `TBTRANSLATEUXONLY`, `TBFMIGNORECASE`, `TBTRANSLATESLASHES` are read (probably `Patch.067C` File Manager).
- Cache mode of the `UI_PHYS_SCREENS` / `UI_ROM` physical mappings.
- Whether the other `$067C` overpatch ROMs (Quadra 610/650/800, Centris) keep the IIci layout at the addresses `Patch.067C` hard-codes.
