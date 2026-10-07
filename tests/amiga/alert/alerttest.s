| alerttest -- t_amiga's alert check: a recoverable alert for five
| seconds, which must show on the card.
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4.w,%a6
 lea intname(%pc),%a1
 moveq #39,%d0
 jsr -552(%a6)
 tst.l %d0
 beq 9f
 move.l %d0,%a6
 move.l #0x00010000,%d0
 lea text(%pc),%a0
 moveq #40,%d1
 move.l #250,%a1
 jsr -822(%a6)
 move.l %a6,%a1
 move.l 4.w,%a6
 jsr -414(%a6)
9: movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
intname: .asciz "intuition.library"
.balign 2
text: .word 120
 .byte 24
 .asciz "Alert test: this alert shows on the card"
 .byte 0
