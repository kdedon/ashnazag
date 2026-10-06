| bsdtest's entry and library call glue
 .text
 .globl _start
_start:
 jsr bsdmain
 rts

| long bcall(base, lvo, regs): regs d0-d3/a0-a3, a6 the base
 .globl bcall
bcall:
 movem.l %d2-%d3/%a2-%a6,-(%sp)
 move.l 32(%sp),%a6
 move.l %a6,%a5
 add.l 36(%sp),%a5
 move.l 40(%sp),%a4
 movem.l (%a4),%d0-%d3/%a0-%a3
 jsr (%a5)
 movem.l (%sp)+,%d2-%d3/%a2-%a6
 rts

 .globl execbase
execbase:
 move.l 4.w,%d0
 move.l %d0,%a0
 rts

 .section .note.GNU-stack,"",@progbits
