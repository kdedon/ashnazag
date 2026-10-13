# Voyage 200

`native` runs a Voyage 200 Flash dump (4 MB: boot code, AMS, archive) from its reset vector
as native 68k code. The tiplatform module supplies the rest: virtual supervisor mode with
68000 exception frames, the `$600000`/`$700000` register pages (LCD base, key matrix,
battery, timers), interrupts from the clock tick, and Flash program/erase applied to the
environment's `flash.img`. Link port and power-off are stubs.

    sh kernel/guest/voyage200/build.sh OUTDIR     # OUTDIR/native

Per environment (`~/Voyage200/<env>`): `media` is the dump, copied once to `flash.img`,
which keeps the archive; `ram` is cleared at each start. Needs an 8-bit display.
Keys: the V200 layout on the host keyboard; Shift, Ctrl = diamond, Option = 2nd,
F9 = APPS, F10 = MODE, Delete = CLEAR, F12 = ON, Cmd-Q quits.

Tested with AMS 2.09 and 3.10 dumps (`GROUP=v200 sh tests/run-qemu.sh`, the dump from
`media/v200.zip`; `V200ROM=voyage200v310.rom` picks the other).
