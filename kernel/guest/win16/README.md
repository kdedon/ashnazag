# startwin

The Windows 3.x environment: one `startwin` process runs one Windows 3.1
session, as Sun's Wabi did on Solaris. The programs' x86 code is
interpreted; KERNEL, USER and GDI are ours, in C; the rest of Windows comes
from the user's own Windows 3.1 or 3.11, and Wabi's own Windows-side files
from the user's own Wabi 2.2, installed with `startwin -install`. Nothing
of Windows or Wabi is in the repository.

Design, use and what is not done: `docs/win16-design.md`. User guide:
`images/README.md`. Tests: `tests/win16/`. Packaging: `images/winenv/mkwin.sh`.

```sh
sh tests/win16/run.sh            # this host: startwin with the memory screen, the tests
sh tests/win16/build.sh out      # the AMIX binary (display service, sound service)
```

## Files

| File | Contents |
|---|---|
| `startwin.c` | the command: options, drives, the session's start and end |
| `install.c` | `startwin -install`: the user's Windows disks or directory, and Wabi 2.2 by its own rules |
| `x86.c`, `x86.h` | the 80386 integer core |
| `x87.c` | the 80387, on the host's floating point |
| `thunk.c` | where x86 code meets native code: API entry points, callbacks |
| `ne.c` | the NE loader: executables, DLLs and our own system modules |
| `apitab.c`, `apitab.h`, `mkapi.py` | every export of the system modules with its arguments (generated) |
| `mem16.c` | guest memory: the linear space, the LDT, the global heap |
| `kernel.c` | KERNEL: tasks, memory, modules, resources, files |
| `task.c` | several programs at once, cooperatively |
| `dos.c` | the DOS a program sees: INT 21h and KERNEL's file calls |
| `profile.c` | WIN.INI and private .INI files |
| `user.c`, `uapi.c` | USER: windows, classes, the message queue, painting; its exports |
| `defwnd.c` | DefWindowProc and the Windows 3.1 frame |
| `dialog.c`, `menu.c`, `controls.c`, `edit.c`, `scroll.c`, `mdi.c` | dialogs, menus, the standard controls, MDI |
| `cursor.c` | cursors and icons |
| `hook.c` | USER's hooks |
| `drv.c` | installable drivers; Wabi's TIMER.DRV wrapped |
| `mmdrv.c`, `snd.h`, `snd_so.c`, `snd_null.c` | the timer and wave output drivers; the sound service (or the clock only, for tests) |
| `ddesetup.c` | Program Manager's groups on its first run, by DDE |
| `gdi.c`, `draw.c`, `rgn.c` | GDI: objects, DCs, mapping, drawing, regions |
| `fontfile.c`, `font.h`, `ttf.c` | the user's bitmap fonts and TrueType |
| `ft/` | FreeType's configuration for TrueType (`w16ftopt.h`, `w16ftmod.h`) and `freetype.sh`, which fetches its pinned release |
| `fonts.c`, `mkfonts.py` | built-in bitmap fonts for when the user's are missing (generated) |
| `obm.c` | the system bitmaps |
| `metafile.c` | metafiles |
| `prn.c` | printing through the printer drivers, the spooler |
| `wabicfg.c` | WABICFG, Wabi's configuration interface, over WABI.INI |
| `ws.c` | WINSOCK.DLL over the host's sockets |
| `other.c` | the small system modules and stand-ins |
| `scr.h`, `scr_fb.c`, `scr_null.c` | the screen and input: the display service, or memory with an input script (tests) |
| `w16.h`, `win.h` | what the parts share |
