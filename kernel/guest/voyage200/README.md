# Voyage 200

`native` runs a Voyage 200 Flash dump (4 MB: boot code, AMS, archive) from its reset vector
as native 68k code. The tiplatform module supplies virtual supervisor mode with 68000
exception frames and the interrupts (host timer, keys). The register pages are memory
holding constants; AMS's routines that need a live answer (key scan, idle halt, Flash
write and erase, LCD off and on) are patched in the running copy to call host services
(DESIGN.md). Link port and power-off are AMS's own over constant registers; OFF blanks the
screen until ON.

    sh kernel/guest/voyage200/build.sh OUTDIR     # OUTDIR/native

Per environment (`~/Voyage200/<env>`): `media` is the dump, read only; each start runs a
patched copy of it. `archive/` holds one file per archived variable in TI's variable file
format (the dump's own archived variables at the first start); the files are laid out in
the copy's archive at start and rewritten as AMS archives, unarchives or deletes. `ram` is
cleared at each start. Needs an 8-bit display.
Keys: the V200 layout on the host keyboard; Shift, Ctrl = diamond, Option = 2nd,
F9 = APPS, F10 = MODE, Delete = CLEAR, F12 = ON, Cmd-Q quits.

Tested with AMS 2.09 and 3.10 dumps (`GROUP=v200 sh tests/run-qemu.sh`, the dump from
`media/v200.zip`; `V200ROM=voyage200v310.rom` picks the other).
