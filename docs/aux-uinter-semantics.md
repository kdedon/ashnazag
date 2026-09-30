# A/UX `/dev/uinter0` semantics

What each `/dev/uinter0` ("user interface" driver) ioctl does.

Sources:
- **Names and structures:** the A/UX header `/usr/include/sys/uinter.h` (`@(#)head:sys/uinter.h 1.60 93/10/06`), which names every ioctl and defines the argument structures and kernel data structures.
- **Behaviour:** analysis of the shipped `/unix` (`ui_ioctl` 0x10088486 and the `UI_*` handlers) and of the callers in `libmac1_s`, `Patch.067C` and `Login`.

Command numbers with kernel handler addresses and user callers are in [aux-uinter-ioctls.md](aux-uinter-ioctls.md). The CPU side (proc flags `SMAC`/`SMAC24`, per-process vectors, A-line and `lpriv` emulation, virtual interrupts) is in [aux-kernel-mac-support.md](aux-kernel-mac-support.md).

Confidence of the behaviour column: **H** = read from code (and consistent with the header), **M** = inferred, **L** = guess.

## Conventions

- BSD-style ioctl encoding, group `'Q'`. The generic ioctl layer copies `_IOW`/`_IOWR` data in and `_IOR`/`_IOWR` data out; `ui_ioctl` receives a pointer to the kernel buffer.
- For `_IO` commands that take a value (`UI_ROM`, `UI_MAP`, `UI_GET_PRODINFO`, …) the value is the first word of that buffer (`ioctl(fd, cmd, value)`).
- Errors are errno: `EINVAL` (22) when there's no layer or the arguments are bad, `EAGAIN` (11), `EEXIST` (17), `EACCES` (13), `ECHILD` (10), `EINTR` (4), `ENOMEM` (12).
- Kernel `copyout` is at 0x556f0 and `copyin` at 0x554f0.

## Configuration and kernel data structures (from `uinter.h`)

- `UI_VERSION 5`, `NDEVICES 1`, `NLAYERS 1`, `NATTACHES 16` (processes per layer), `NEVENTS 32` (queued events per layer).
- There is one device and one layer. The layer is the Mac virtual machine; processes attached to it are the Mac "tasks".

### u-area word (u-area 0x12fff52c)

| Macro | Bits |
|---|---|
| `UI_LAYER(x)` | bits 0–7; `NOLAYER` = 0xff |
| `UI_DEVICE(x)` | bits 8–14 |
| `UI_FLAG` | 0x8000, "attached to a user interface" |
| `UI_GETID(x)` | bits 16–23, index into `l_attached[]` |

`open()` sets the word to 0x80ff.

### `struct layer` (`ui_layer` at 0x1205a65c, sizeof 0x6b6)

| Off | Field | Use |
|---|---|---|
| 0x00 | `l_state` {state, wanted} | `LS_EMPTY` 0 / `LS_DONE` 1 / `LS_INUSE` 2. wanted: `C_WANTED` 1 cursor, `A_WANTED` 2 input, `PHANTOM_FLAG` 4 |
| 0x02–0x09 | `l_update`, `l_down`, `l_char`, `l_timeout`, `l_time` | update pending, auto-repeat state |
| 0x0a / 0x0e / 0x12 | `l_first` / `l_last` / `l_free` | event queue lists (`struct event`: next + `EventRecord`) |
| 0x16 | `l_mask` | event mask: `AUX_EVENT_MASK` (0x7fff0000) OR'd with `SysEvtMask` |
| 0x1a | `l_sleep` | events being slept for; `waitMask` 0x80000000 = in `WaitNextEvent` |
| 0x1e | `l_mouse` | `WaitNextEvent` mouse rect |
| 0x26 | `l_selmouse` | `select()` mouse rect |
| 0x2e | `l_active` | current `struct attach` (the running Mac task) |
| 0x32 | `l_prevtime` | last `time` seen by `ui_update` |
| 0x36 | `l_trunning` | tick timer running |
| 0x3a | `l_wakeme` | `lbolt` deadline for a sleeping `UI_DELAY` |
| 0x3e | `l_shmid` | low-memory shm id |
| 0x42 / 0x46 | `l_gofptr` / `l_gpfptr` | global file table (`struct file *[]`, flags) |
| 0x4a | `l_refcnt` | number of attached tasks |
| 0x4c | `l_killed` | |
| 0x4e | `l_romaddr` | user address of the ROM mapping |
| 0x52 | `l_lowaddr` | |
| 0x56… | `l_screen[NSCREENS]`, `l_screen24[]`, `l_screenRBV` | `scrninfo` {vaddr, paddr, size} |
| … | `l_events[32]` | event storage |
| 0x372 | `l_attached[16]` | `struct attach`, 0x30 bytes each |
| 0x672 | `l_processes[16]` | |
| 0x6b2 | `l_maxproc` | |

### `struct attach` (one per Mac task)

| Field | Meaning |
|---|---|
| `procp` | the task's proc |
| `select` | `struct select` {nfd, mask[3]}: fds whose readiness should wake the task |
| `pid` | +0x14 |
| `brkselect` | |
| `romid` | +0x17, phys slot of the ROM mapping |
| `screenid[]`, `screenid32[]`, `screenid24[]`, `screenidRBV` | per-screen phys slots |
| `savedusp` | |

### `struct ui_interface` (the shared page)

Mapped by `UI_MAP` at user 0x3000 (`uip`). Holds:
- mouse and cursor positions: `c_mx`, `c_my`, `c_cx`, `c_cy`
- screen geometry and hot spot
- cursor mask and saved-under data (1–32 bpp), `c_style`, `c_newcrsr`, `c_lock`
- `c_button`, `c_modifiers`, the mouse acceleration table `c_mlookup[10]`, and the auto-key threshold and rate

The kernel's cursor code (`uinters.s`, run at vertical retrace) and the Mac side share it. In the disassembly, +0x458 is where `UI_MAP` seeds `c_button`, and `UI_SWITCH` writes the current task's pid at +0xff8, outside `struct ui_interface`.

### Aux event codes

Posted into the Mac event queue by the kernel: `attachEvt` 16 (a task attached), `exitEvt` 17, `selEvt` 18 (a watched fd became ready), `waitEvt` 31 (dummy, timeout).

## Device entry points

| Entry | Behaviour | Conf. |
|---|---|---|
| `open` | Minor must be 0 and below `video_count`. The first open installs `ui_callout = ui_update`, `ui_setsched = UI_setsched` and `ui_sigpending = UI_sigpending` (see the kernel doc, "Virtual interrupts"). It clears the layer, patches the `cdevsw` select entry to `ui_select`, and sets `postDIroutine = postDIevent` (disk-insert events). Sets the caller's u-area word to 0x80ff | H |
| `read`, `write` | Always `EINVAL`. Events come through ioctls, not read | H |
| `select` | The `WaitNextEvent` sleep. Readable if the mouse (low memory `$82C`) is outside `l_selmouse`, or if a non-blocking `UI_GETOSEVENT` finds an event. Otherwise it arms `ui_seltimer` with the layer timeout and returns not ready | H |
| `close` | open item: behaviour not yet traced | — |

## Version handshake

`openDevices` (`libmac1_s`) opens `/dev/uinter0` O_RDWR and `/dev/console`, then calls `ioctl(fd, UI_GETVERSION, 0)`. The kernel returns `UI_VERSION` = 5 as the call's return value, and the Toolbox requires exactly 5. Otherwise it prints "This version of the Toolbox is incompatible with the user interface driver in the kernel. Toolbox version = 5, driver version = N" (or "prehistoric" if the call fails) and exits.

## Startup sequence (`libmac1_s` `bt_7`)

The full boot is in [aux-startmac-boot.md](aux-startmac-boot.md). The uinter part: `mode` ≠ 0 creates the VM (`startmac`); `mode` = 0 attaches to it (CommandShell, `launch`, `setfile`, other COFF Mac programs).

1. `UI_GETVERSION`.
2. Create: `UI_TEST(0)`. Attach: `UI_SYNC(0)`.
3. Shared-memory segments (`openSegments`).
4. Create only:
   - `memset(0, 0xff, 0x4000)`
   - `UI_UNMAP`, `UI_MAP(0x3000)`, `UI_CREATELAYER`
   - `UI_SHMID(id)`, or `IPC_RMID` when mode is 2
   - `UI_ATTACHGFD`
   - `UI_PHYS_SCREENS` (`mapScreens`)
   - `UI_ROM(0x50000000)`, copy to shm at 0x40800000, `UI_UNROM` (24-bit: 0xf00000 → 0x800000)
   - `UI_SET`, then load `Patch.<rom>`
5. Attach only: `UI_SETCOFFNAME`, `UI_ATTACHLAYER` (`EACCES` → "cannot be used in 24-bit mode"), `UI_SET`.

## Ioctls

"Ess." marks what a host must implement. **Y** = needed to bring up `startmac`. **P** = needed for multi-process pass-through (CommandShell, COFF Mac apps, drives, SCSI). **S** = can be stubbed. **N** = not issued by the Mac environment binaries examined.

### VM setup, memory, layers

| # | Name | Arg (`uinter.h`) | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 0 | `UI_GETVERSION` | – | Returns `UI_VERSION` (5) | Y | H |
| 1 | `UI_SET` | value (1) | "Set the A-line trap handler". First call locks user 0..0x3000 (Mac low memory) in core and builds the kernel view (`ui_lowaddr`, `ui_memmap`). Installs the Mac vectors for the process (see the kernel doc, "Per-process exception vectors") | Y | H |
| 2 | `UI_CLEAR` | – | Restores the normal vectors and drops the low-memory mapping | Y | H |
| 4 | `UI_UNSCREEN` | – | Unmaps the screens | S | M |
| 5 | `UI_ROM` | virtual address (segment-aligned) | Maps physical ROM (0x40000000, size from `getROMSize`) at that address through the `phys()` mechanism. Stores `l_romaddr` and `attach.romid` | Y | H |
| 6 | `UI_UNROM` | – | Unmaps this task's ROM mapping; clears `l_romaddr` when none are left | Y | H |
| 7 | `UI_MAP` | user address (0x3000) | Maps in and locks one page for `struct ui_interface` (cursor and mouse). One per open. `EINVAL` if already mapped | Y | H |
| 8 | `UI_UNMAP` | – | Releases devices and the cursor if held, then unlocks the page | Y | H |
| 11 | `UI_PHYS` | `struct ui_phys` {vaddr, paddr, size} | Maps arbitrary physical memory (size ≤ 0x10000000). Returns the phys slot | N | H |
| 21 | `UI_CREATELAYER` | – | Needs `UI_MAP` and no current layer. Creates the layer, sets the u-area word (`UI_FLAG` etc.), sets `SMAC` on the proc, returns the layer id (0) | Y | H |
| 22 | `UI_UNLAYER` | – | Detaches (u-area word = 0x80ff), wakes waiters | S | M |
| 23 | `UI_SETLAYER` | int layer | Makes the layer the active one (`ui_active`), wakes waiters | Y | M |
| 33 | `UI_PUSHLAYER` | – | Brings another layer in front of this one | N | M |
| 34 | `UI_PHYS_SCREENS` | `struct screens`: `screen[NSCREENS]` {dCtlSlot, dCtlSlotId, dCtlExtDev, dCtlDevBase}, slot −1 = invalid | Maps each video device's NuBus slot space (16 MB at `0xFs000000` in 32-bit, 1 MB in 24-bit) and fills in the table | Y | M |
| 36 | `UI_ATTACHGFD` | – | Allocates the layer's global file table (`l_gofptr`, `l_gpfptr`, GNOFILE entries) and installs it in the u-area, so all tasks in the VM share Mac-side open files | Y | M |
| 37 | `UI_ATTACHLAYER` | `struct attachInfo` {layerid, size, flags} | Joins a running VM (`LS_INUSE`) as a new task. The `SMAC24` setting must match the VM's (`EACCES` otherwise). Takes a free `l_attached[]` slot (max 16), maps ROM and screens, installs the global file table, sets `UI_SETID(slot)`, posts `attachEvt` (pid, size, flags) to the VM, then sleeps until scheduled. Returns the slot | P | H |
| 43 | `UI_KILLMYLAYER` | – | Terminates the VM (`ui_terminate`); used at logout | S | M |
| 46 | `UI_SHMID` | int | Records the low-memory shm id (`l_shmid`) so attaching tasks can `shmat` it | Y | H |
| 50 | `UI_SYNC` | int layer | Waits (20 s timeout) until the layer is `LS_INUSE` and active; `EAGAIN` otherwise | P | H |
| 52 | `UI_TEST` | int layer | `EEXIST` if the layer is in use, else 0 | Y | H |
| 63 | `UI_SETCOFFNAME` | `struct coffname` {taskid, path, pathlen} | Records the COFF path for a task (`CoffNames`) | P | M |
| 64 | `UI_GETCOFFNAME` | same | Reads it back; used for `AUX_COFFFSSPEC` | P | M |

### Task scheduling inside the VM

| # | Name | Arg | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 38 | `UI_SWITCH` | int pid (negative = caller is leaving) | Makes the task with `abs(pid)` the running one (`l_active`) and writes its pid to ui page +0xff8. If the target was stopped it sends SIGIOT, then wakes it. With a negative pid the caller gets SIGKILL or SIGTERM; otherwise the caller sleeps until switched back. **One Mac task runs at a time**; this implements the Process Manager's `AUX_SwitchGlue` | P | H |
| 39 | `UI_SLEEP` | – | Sleeps until the caller is `l_active` | P | H |
| 40 | `UI_SELECT` | `struct select` {nfd, mask[3]} | Stores the fd set (in `attach.select`) whose readiness wakes this task and posts `selEvt`. Used for SIGIO-free I/O waiting (`ui_setselect`) | Y | M |
| 41 | `UI_GETTASKID` | out long | **Not dispatched** by this kernel's `ui_ioctl` (returns `EINVAL`) | N | H |
| 27 | `UI_HASKIDS` | int pid | 0 if some process has that pid as parent, else `ECHILD` | P | H |

### Events and input

| # | Name | Arg | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 9 | `UI_CURSOR` | – | Starts kernel cursor display on vertical retrace (`C_WANTED`) | Y | H |
| 10 | `UI_UNCURSOR` | – | Stops it | Y | H |
| 12 | `UI_DELAY` | long in/out | Sleeps until `Ticks` (low memory `$16A`) reaches the value (user side passes `Ticks + n`). Returns ticks since boot. `EINTR` if interrupted | Y | H |
| 16 | `UI_POSTEVENT` | `struct postevent` {eventCode, eventMsg} | Posts to this layer if the code is in the mask | Y | H |
| 17 | `UI_LPOSTEVENT` | `struct lpostevent` {layer, eventCode, eventMsg} | Posts to a named layer | N | H |
| 18 | `UI_FLUSHEVENTS` | `struct flushevents` {eventMask, stopMask} | `FlushEvents` | Y | H |
| 19 | `UI_GETOSEVENT` | `struct getosevent` {blocking, auxevents, eventMask, theEvent, timeOut, mouseRect} | Gets an event. `blocking`: `NOBLOCK` 0, `BLOCK` 1 (until event, timeout or mouse leaves `mouseRect`), `AVAIL` 2 (peek), `AVBLOCK` 3. `auxevents` enables aux codes 16–18 | Y | H |
| 20 | `UI_SETEVENTMASK` | short | `l_mask = AUX_EVENT_MASK | mask`; the user side mirrors `SysEvtMask` ($144) | Y | H |
| 24 | `UI_DEVICES` | – | Turns keyboard and mouse on for the layer (`A_WANTED`), installing `ui_key_intr`/`ui_mouse_intr` | Y | H |
| 25 | `UI_UNDEVICES` | – | Turns them off | Y | H |
| 26 | `UI_SETSELRECT` | `Rect` | Sets `l_selmouse` for `select()` | Y | H |
| 30 | `UI_POST_MOD` | `EventRecord` | Posts an event including modifiers | P | H |
| 31 | `UI_FIND_EVENT` | `struct findevent` {EventRecord mask; EventRecord data} | Searches the queue for a matching event | P | M |
| 51 | `UI_SET_KCHR` | `struct kybd_map` {base_address, byte_count} | Loads the KCHR keyboard layout used by the kernel's key translation (≤ 0xc00 bytes) | Y | M |
| 53 | `UI_MOUSE` | `struct mouse_info` {x, y, button} | "Force mouse" (VU/Mole). **Not dispatched** by this kernel | N | H |
| 54 | `UI_KEYBOARD` | char | Forces a keycode through the keyboard path | N | H |
| 55 | `UI_POST_EVTREC` | `EventRecord` | Posts a whole event record | P | H |
| 69 | `UI_GETKEYS` | user pointer to `keyarray_t` (128 bytes) | Copies out the key-down state (`GetKeys`) | Y | H |

### Machine info, PRAM, drives, video, SCSI

| # | Name | Arg | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 28 | `UI_READPRAM` | `struct pram` {buffer, count}; `count` = (length << 16) \| offset, as in the Mac XPRAM convention, length + offset ≤ 256 | Kernel `ReadXPRam`, then copies out | Y (or fake) | H |
| 29 | `UI_WRITEPRAM` | same | Copies in, then `WriteXPRam` | S | H |
| 32 | `UI_COPY_OUT` | `struct lm_element` {start_address, byte_count}, within 0..0x3000 | Copies a region of the **kernel's** low memory (set up by the ROM at boot, see the kernel doc) to the same user address | Y | H |
| 35 | `UI_TIMER` | long in/out | Starts the layer tick (`l_trunning`): a 1-tick kernel timeout (`ui_catch_timer`) that re-arms itself, wakes a sleeper whose `l_wakeme` passed, and sends **SIGIOT (6)** to the running task every tick. The header comment ("go off in n ticks … generate a SIGSEGV") is out of date; the code ignores n. See the kernel doc, "Virtual interrupts" | Y | H |
| 42 | `UI_GETDQEL` | `DrvQEl` (the kernel reads the index from offset 0xa, `dQFSID`, and returns `struct drive_queue` {flags; DrvQEl}) | Returns the n-th Mac drive-queue element held by the kernel (`kernel_info` + 0xb2) | Y | M |
| 65 | `UI_PUTDQEL` | `DrvQEl` | Adds an element | P | M |
| 66 | `UI_DELDQEL` | `DrvQEl` | Removes an element | P | M |
| 47 | `UI_VIDEO_CONTROL` | `CntrlParam` (50 bytes); video index in `ioResult` position (+4, result returned there), `csCode` +0x1a, `csParam` +0x1c | Runs the video card's declaration-ROM driver Control call in the kernel (`callControl`). See the kernel doc, "ROM code run later" | Y | H |
| 48 | `UI_VIDEO_STATUS` | same | Status call (`callStatus`) | Y | H |
| 56 | `UI_GETKIFLAGS` | out ushort | `kernel_info.ki_flags` (`kernel_info` + 0xb6). `Patch` uses it for `GetParityAttr` | S | H |
| 67 | `UI_GET_PRODINFO` | user pointer | Copies out 0x34 bytes of the ProductInfo record (kernel low memory `$DD8`) | Y | H |
| 68 | `UI_GET_INTERR_VECTORS` | user pointer | "Get saved Mac floating-point exception vectors": on machine type 5 copies out 0x14 bytes of `mac_interr_vecs`, else returns 1 | Y | H |
| 70 | `UI_GETSCSIID` | user pointer | Copies out 0x1c bytes of `scsiarray`: the first 7 SCSI devices and which the Mac side may use | P | H |
| 71 | `UI_SCANSCSI` | – | Rescans the SCSI buses (`ss_scan`) | N | H |

### 7.0 VM calls (`cMemoryDispatch` in `Patch.067C`)

| # | Name | Arg | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 57 | `UI_VM_HOLDMEM` | `struct vm_lock` {address, count} | `HoldMemory` | S | H |
| 58 | `UI_VM_UNHOLDMEM` | same | `UnholdMemory` | S | H |
| 59 | `UI_VM_LOCKMEM` | same | `LockMemory` (shares the handler with #57, which gets the command) | S | H |
| 60 | `UI_VM_UNLOCKMEM` | same | `UnlockMemory` (shares with #58) | S | H |
| 61 | `UI_VM_LOCKMEMCONTIGUOUS` | same | `LockMemoryContiguous` | S | M |
| 62 | `UI_VM_GETPHYS` | `struct vm_phys` {LogicalToPhysicalTable *table, entrycount} | `GetPhysical` | S (unless DMA drivers) | M |

### Power

| # | Name | Arg | Behaviour | Ess. | Conf. |
|---|---|---|---|---|---|
| 44 | `UI_REBOOT` | – | `sys_shutdown(0x4a530060)` | S | H |
| 45 | `UI_SHUTDOWN` | – | `sys_shutdown(0x4a530061)` (power off) | S | H |

## Kernel work outside the ioctls

Covered in [aux-kernel-mac-support.md](aux-kernel-mac-support.md) and [aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md). Summary of what a replacement must reproduce:
- **`ui_update` clock hook**: keeps `Ticks` ($16A), `Time` ($20C) and the alarm state ($200/$208/$21F) current in Mac low memory.
- **Input interrupts**: post events and move the cursor in `struct ui_interface`.
- **Signal gating**: signals are held by the virtual IPL.

## Minimum set for a host implementation

Bringing up `startmac` alone:
- setup: #0, #52, #1, #2, #5, #6, #7, #8, #21, #23, #34, #36, #46
- events: #9, #10, #12, #16, #18, #19, #20, #24, #25, #26, #40, #51, #69
- machine: #28, #32, #35, #42, #47, #48, #67, #68
- `select()` and the clock hook
- the `SMAC` vector and virtual-IPL machinery from the kernel doc

Multi-process pass-through (CommandShell, COFF Mac apps, drives, SCSI) adds #27, #30, #31, #37, #38, #39, #50, #55, #63, #64, #65, #66, #70.

Everything else can be stubbed or left out (#11, #17, #33, #41, #53, #54, #71 aren't used by these binaries).
