# The Win16 environment

`startwin` runs Windows 3.x programs on Ash Nazag the way Sun's Wabi ran them
on Solaris: no Windows underneath, no PC emulator. One `startwin` process is
one Windows session. The programs' x86 code is interpreted; the core of
Windows (KERNEL, USER, GDI) is ours, written in C and running as native 68k
code, drawing through the display service. Everything else of Windows comes
from the user's own copy of Windows 3.1 or 3.11, installed into their home
directory. Nothing of Windows or Wabi is in this repository, as nothing of
TOS or Mac OS is.

Sources: `kernel/guest/win16/`. Tests: `tests/win16/`. Packaging:
`images/winenv/mkwin.sh`.

## What is ours and what is the user's

Wabi 2.2's own installer (`wabi_f.lst`, read from a local copy, not kept)
settles the split. It copies the user's Windows 3.11 disks except
`gdi.exe`, `user.exe`, `timer.drv` and `toolhelp.dll`, and its SYSTEM.INI
points the drivers at its own. We draw the line in the same place:

| Ours (native code) | From the user's Windows |
|---|---|
| KERNEL, USER, GDI (with its TrueType rasterizer), TOOLHELP, WINSOCK | Program Manager, File Manager, Control Panel and the applets |
| the drivers: DISPLAY, KEYBOARD, MOUSE, SYSTEM, SOUND (they program PC hardware), TIMER.DRV and the wave output ASHAUDIO.DRV under MMSYSTEM | COMMDLG, SHELL, DDEML, OLECLI/OLESVR, VER, LZEXPAND, MMSYSTEM, MCIWAVE, MMTASK, REGEDIT |
| the x87 (`x87.c`) | WIN87EM.DLL (passes through to our x87) |
| | the fonts (`*.FON`, `*.FOT`/`*.TTF`), WIN.INI, SETUP.INF, SETUP.REG, the help viewer |

When the user's files are missing, small stand-ins (SHELL, VER, LZEXPAND,
MMSYSTEM, COMMDLG stubs in `other.c`) and eleven built-in fonts made from
X.Org's Adobe 100 dpi fonts (`fonts.c`, MIT/X licence) let simple programs
run.

## Installing the user's Windows

```sh
startwin -install /path/to/DISK1.IMG /path/to/DISK2.IMG ...
startwin -install /path/to/disks-or-an-installed-WINDOWS-directory
```

`install.c` reads FAT floppy images directly, or directories (the disks'
files together or in `DISK1`, `DISK2` ...), or an installed Windows
directory copied from a PC. Compressed files are expanded: SZDD (`COMPRESS
-r`) and KWAJ (stored, XOR, LZSS and the LZ+Huffman method of the 3.11
disks; MSZIP is not handled). SETUP.INF decides WINDOWS or SYSTEM for each
file, the extension otherwise. Left out: our own modules, the 386 enhanced
mode VxDs and its Control Panel applet, DOS swappers and WINOLDAP, PC
drivers. WIN.SRC becomes WIN.INI with Setup's `[intl]` section and its
`[fonts]` (the VGA bitmap fonts, the plotter fonts and the TrueType fonts
of `[ttfonts]`); SYSTEM.INI is ours, with Setup's multimedia `[drivers]`
and `[mci]`; SETUP.INF stays in SYSTEM. Everything goes to `~/WIN16/windows`
(drive C: is `~/WIN16`).

On the first session (no REG.DAT yet) the user's `REGEDIT /S SETUP.REG`
runs before the first program, as Setup runs it: OLE servers such as
Paintbrush register in the database it fills.

On Program Manager's first run (no PROGMAN.INI), `ddesetup.c` makes the
groups as Setup did: the `[progman.groups]` sections of SETUP.INF sent to
Program Manager as DDE commands (CreateGroup, AddItem, ShowGroup), skipping
programs that are not installed and the DOS prompt.

## Running

```sh
startwin                      # Program Manager (C:\WINDOWS\PROGMAN.EXE)
startwin notepad.exe file.txt # one program; its directory becomes D: if outside the drives
startwin -C dir ...           # another C: than ~/WIN16
```

Drives: C: `~/WIN16`, H: `$HOME`, R: `/`, D: a program's own directory when
it is outside these. `-D X=dir` adds one. `-m MB` sets the 16-bit memory
(default 8). `-g WxH` sizes the screen for the memory backend. Under xdm it
is the session "Windows 3.x programs (Win16)" (`xdmenv win`).

Debugging: `-v` logs loads, `-vv` every API call and DOS call, `-vvv`
arguments and results too; `W16_BT=Name` prints the x86 call chain when API
Name is called; `W16_TRACE=MODULE:seg[:from-to]` logs each instruction
there (seg 0: all of the module); `W16_WATCH=sel:off` reports the API call
that changed a guest word; with the memory screen, `W16_SND=file` keeps
the sound played (Windows' sample formats, as given).

## Architecture

- **x86 (`x86.c`)**: an 80386 integer core, 16-bit segments, protected mode
  through a local descriptor table the host owns (Win16 selectors), real
  mode for the tests. Lazy flags; faults by longjmp. Checked against
  SingleStepTests' 80386 suite (1,758,600 cases pass; the 35 differences
  are listed in `tests/win16/x86known.txt`). The thunk opcode `0F FF lo hi`
  calls host function `lo|hi<<8`; every API entry point is one.
- **x87 (`x87.c`)**: the 80387 on the host's floating point. Registers are
  `long double` (the 68881's extended format has the x87's 64-bit
  significand); arithmetic is the compiler's; transcendental functions are
  libm's. On a Quadra that is the FPU; on the Falcon AMIX's F-line emulator
  stands in, as for any program. Programs see a coprocessor (WF_80x87), so
  the loader leaves their x87 code alone; WIN87EM.DLL passes through.
- **Loader (`ne.c`)**: NE executables and DLLs, every segment loaded at once
  and kept, relocations, the entry table, prologue patching as Windows does
  it, resources on demand. Our modules answer for KERNEL, USER, GDI and the
  drivers whatever the disk holds; any other import comes from the disk
  (NAME.DLL searched before NAME.EXE).
- **API (`apitab.c`, made by `mkapi.py`)**: every export of the 3.1 system
  modules with its Pascal argument list, from Wine's `.spec` files
  (interface facts). An export we do not implement returns 0 and is logged
  once (`not done:`); `-strict` makes that fatal.
- **KERNEL (`kernel.c`, `mem16.c`, `dos.c`, `profile.c`, `task.c`)**: the
  global heap (handle = selector; huge blocks; discarding), local heaps with
  moveable handles, resources, atoms, profiles, files through a DOS layer
  (INT 21h subset, 8.3 names matched case-blind on the host), DPMI subset,
  Catch/Throw, interrupt vectors set by programs.
- **Tasks (`task.c`)**: several programs at once as Windows 3.1 runs them,
  cooperatively. Each task has its own host stack (SVR4 `ucontext`), x86
  registers, x87, callback stack and USER scratch; it yields only in
  GetMessage, PeekMessage, Yield, WaitEvent and WinExec, or when it ends.
  Messages, timers, painting, task events (WaitEvent, PostEvent: MMSYSTEM's
  mmTaskBlock and mmTaskSignal) and WM_QUIT are per task. A second instance
  of a running program loads as its own copy of the module. The scheduler
  keeps its own state and a stack of its own for what runs outside any task
  (DLL initialisation). When the first task (the shell) ends, or
  ExitWindows has asked every window (WM_QUERYENDSESSION), the session
  ends.
- **USER (`user.c`, `defwnd.c`, `menu.c`, `dialog.c`, `controls.c`,
  `edit.c`, `scroll.c`, `mdi.c`, `cursor.c`, `uapi.c`)**: windows, the raw
  input queue converted to messages as they are taken, DefWindowProc with
  the Windows 3.1 frame, menus, dialogs and MessageBox, the standard
  controls, MDI, icons and cursors kept in global memory in Windows' own
  layout (programs lock and fill them), clipboard, timers, caret, WinHelp
  (starts the user's WINHELP.EXE), installable drivers (`drv.c`: OpenDriver
  and the rest; SYSTEM.INI's `[boot] drivers=` opened at the start, as USER
  does), MessageBeep and MessageBox playing WIN.INI's `[sounds]`.
- **GDI (`gdi.c`, `draw.c`, `rgn.c`, `fontfile.c`, `ttf.c`, `obm.c`)**: an
  8-bit palette device (the 20 static colours at the ends, a 6x6x6 cube,
  greys; brush colours the palette lacks dithered from the 16 VGA colours,
  as Windows 3.1's drivers do), DCs, mapping modes, regions, ROP2 and ROP3,
  DIBs and DDBs, Windows' own `.FON` bitmap fonts (96 dpi sizes preferred)
  and TrueType: the user's `.TTF` files, named in WIN.INI by their `.FOT`
  headers, scan converted at any size when first drawn (nonzero rule,
  dropout control, no hinting; 26.6 integer arithmetic for a 68k without an
  FPU). The system bitmaps (OBM_*, the display driver's in Windows) are
  drawn with the frame's routines. The spooler's GetSpoolJob lists WIN.INI's
  printers to Print Manager.
- **Screen (`scr.h`)**: `scr_fb.c` on the display service (a session on
  `/dev/fb0`: an 8-bit shadow copied to packed 1/4/8/16/32-bit or
  interleaved-plane frame buffers, a software pointer; `/dev/kbd` and
  `/dev/mouse` bound to the session, ADB, Amiga and IKBD key codes to
  virtual keys); `scr_null.c` in memory, with a script for input and PPM
  screenshots, for the tests.

## Drivers

No new kernel driver is needed. Wabi shipped one (`wabi.o`), and it only
handed out NFS file handles (WabiGetfh) so Wabi could identify files for
DOS file locking across processes; one `startwin` session locks within its
own process, and `fcntl` locks would serve across sessions if that is ever
wanted. Video, keyboard and mouse use the display service as the TOS and
Amiga environments do.

Sound goes to the host's sound service through `kernel/guest/sndout.c`, as
the TOS and Amiga environments play theirs (`snd_so.c`; `snd_null.c` keeps
time for the tests). The user's MMSYSTEM opens our drivers by SYSTEM.INI's
`[drivers]`: `timer=timer.drv` (its clock and timeSetEvent) and
`wave=ashaudio.drv` (`mmdrv.c`: wave output, 8 or 16 bits, mono or stereo,
4-48 kHz; buffers fed a lead ahead of the clock and finished through
MMSYSTEM's DriverCallback, from the message loop and between API calls,
which stand for Windows' interrupt time). MCIWAVE (Media Player, Sound
Recorder's files) plays through it in its MMTASK task. SOUND.DRV's voice
API (Windows 3.0) plays square-wave notes on the same stream. There are no
recording, MIDI or auxiliary devices (MCI's sequencer opens but has no
output to play to).

## Networking

WINSOCK.DLL is ours (`ws.c`), as Wabi 2's was: Windows Sockets 1.1 over
the host's BSD sockets (on AMIX libsocket and libnsl), so the host's
network and the user's rights apply; a third-party WINSOCK.DLL in the
user's Windows directory is not used. Sockets are nonblocking underneath.
A blocking call waits as Windows' stacks did, the blocking hook running
(the program's, or the default that dispatches its messages) until the
socket is ready or WSACancelBlockingCall; WSAAsyncSelect's events are
found from the message loop and posted, each once until the call that
re-enables it; the asynchronous database calls are answered at once by
message. Stream and datagram sockets of AF_INET, the usual socket options,
select, the host and service databases.

## Testing

`sh tests/win16/run.sh` (on any host with a C compiler; Open Watcom 2 for
the test programs, `WATCOM`): builds `startwin` with the memory screen,
checks `-install` on synthetic compressed disks, builds and runs the test
programs in `tests/win16/src` with their input scripts, and compares what
they report (`RESULT.TXT`) and their exit codes with `tests/win16/expect`.
The x87 test compares inline x87 results with independently computed ones;
the voice test checks SOUND.DRV's samples (length and pitch); the sock test
runs Winsock over the loopback: a connection by blocking calls, select,
WSAAsyncSelect's events and an asynchronous lookup.
`sh tests/win16/x86suite.sh` runs the 80386 instruction suite.
`sh tests/win16/build.sh out` cross-builds the AMIX binary.

With the user's Windows 3.11 (not in the repository), on the memory screen:
Program Manager with its groups and icons, launching programs and Exit
Windows; Notepad, Write, Cardfile, Calendar, Calculator, Clock, Solitaire,
Minesweeper, PIF Editor, Clipboard Viewer, Task List, Control Panel, File
Manager, Paintbrush, Character Map, Windows Help, Print Manager, Recorder,
Object Packager, Sound Recorder and Media Player (playing through our wave
driver), Terminal (up to its port settings).

## Not done yet

- The AMIX binary is not built or timed on a Quadra here (no AMIX toolchain
  in the container used): `tests/win16/build.sh` and `images/winenv/mkwin.sh`
  are written to the repository's conventions but unrun.
- Serial ports (COMM.DRV's OpenComm and the rest over the host's ttys:
  Terminal), printing (printer drivers and the spooler's jobs), MIDI
  output, recording.
- TrueType hinting, scaled raster fonts (a bitmap face stretched to a size
  it lacks), the clipboard's formats beyond text, DDEML's advanced paths,
  the MDI client's scroll bars, scaled cursors.
