.text
.globl _start, register_boot
/* C ABI: register_boot(ConfigDev *, handler BPTR). */
_start:
register_boot:
 movem.l %d2-%d4/%a2-%a3/%a6,-(%sp)
 move.l 28(%sp),%a2
 move.l 32(%sp),%d2
 moveq #0,%d4
 cmpa.l #0,%a2
 beq done
 tst.l %d2
 beq done
 move.l 4,%a6
 lea expansion_name,%a1
 moveq #36,%d0
 jsr -552(%a6)
 tst.l %d0
 beq done
 move.l %d0,%a3
 moveq #52,%d0
 move.l #65537,%d1
 jsr -198(%a6)
 tst.l %d0
 beq close
 move.l %d0,%d3
 move.l %d0,%a0
 move.l #16384,20(%a0)
 move.l #5,24(%a0)
 move.l %d2,32(%a0)
 move.l #-1,36(%a0)
 lea 44(%a0),%a1
 move.l #0x044d4947,(%a1)
 move.b #0x30,4(%a1)
 move.l %a1,%d0
 lsr.l #2,%d0
 move.l %d0,40(%a0)
 moveq #127,%d0
 moveq #1,%d1
 move.l %a2,%a1
 move.l %a3,%a6
 jsr -36(%a6)
 move.l %d0,%d4
 move.l 4,%a6
 tst.l %d4
 bne close
 move.l %d3,%a1
 moveq #52,%d0
 jsr -210(%a6)
close:
 move.l %a3,%a1
 jsr -414(%a6)
done:
 move.l %d4,%d0
 movem.l (%sp)+,%d2-%d4/%a2-%a3/%a6
 rts
expansion_name: .asciz "expansion.library"
.balign 4
