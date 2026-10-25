# nextguest

Loadable module behind `/dev/nextguest` (major 67): the NeXT turbo board for the `native`
host in `kernel/guest/next`. The board's I/O window is memory holding the ids the kernel reads;
the kernel's own timer, clock-chip, time and software-interrupt routines are patched by the host
program to call host services here (`nextdev.c`). Interrupts reach the guest as vectored exceptions
from the clock tick. An access nothing backs is the guest's bus error, counted (`nextfault.c`).
Floating-point instructions and data types the 68040 leaves to software (packed decimal, transcendentals) go to the
host's FP package, so the guest's own never runs.
