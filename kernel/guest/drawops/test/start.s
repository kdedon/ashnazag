| a program without a C library under Linux user emulation
 .globl _start, sys_write, sys_exit
_start:
 move.l (%sp),%d0
 lea 4(%sp),%a0
 lea 4(%a0,%d0.l*4),%a1
 move.l %a1,-(%sp)
 move.l %a0,-(%sp)
 move.l %d0,-(%sp)
 jsr main
 move.l %d0,%d1
 moveq #1,%d0
 trap #0
sys_write:
 movem.l %d2-%d3,-(%sp)
 moveq #4,%d0
 move.l 12(%sp),%d1
 move.l 16(%sp),%d2
 move.l 20(%sp),%d3
 trap #0
 movem.l (%sp)+,%d2-%d3
 rts
 .section .note.GNU-stack,"",@progbits
