.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l %a0,-(%sp)
 jsr fltest
 addq.l #4,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
/* U call(base, lvo, regs): regs d0-d3, a0-a2 in; returns d0 */
.globl call
call:
 movem.l %d2-%d3/%a2/%a6,-(%sp)
 pea 1f(%pc)
 move.l 24(%sp),%a6
 move.l %a6,-(%sp)
 move.l 32(%sp),%d0
 add.l %d0,(%sp)
 move.l 36(%sp),%a0
 movem.l (%a0),%d0-%d3/%a0-%a2
 rts
1: movem.l (%sp)+,%d2-%d3/%a2/%a6
 rts
