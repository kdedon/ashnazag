# nextguest

Loadable module behind `/dev/nextguest` (major 67): the NeXT turbo board for the `native`
host in `kernel/guest/next`. Register accesses fault; `nextfault.c` decodes the instruction
(data moves, arithmetic, bit fields, MOVE16, MOVEM) and performs it on the register model
in `nextdev.c`. Interrupts reach the guest as vectored exceptions from the clock tick.
`nextdev.c` (interrupt status and mask, timer, event counter, system control, clock chip and NVRAM) derives from the Previous emulator (GPL-2.0-or-later); see THIRD_PARTY.md.
