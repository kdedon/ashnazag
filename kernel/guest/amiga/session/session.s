/* Workbench Tools menu items: Log Out ends the Amiga session; root also
 * gets Shut Down, which the host refuses to anyone else.  Started from
 * WBStartup, it keeps the items for the session.  Host calls are trap #0. */
.equ INPUT, 0x21000000
.equ INPUT_MAGIC, 0x4d494e50
.equ AMIGAIOC_HALT, 0x4108
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4,%a6
 sub.l %a1,%a1
 jsr -294(%a6)
 move.l %d0,%a4
 moveq #0,%d7
 sub.l %a3,%a3
 sub.l %a5,%a5
 moveq #0,%d4
 /* from Workbench: its startup message, replied as we leave */
 tst.l 172(%a4)
 bne 1f
 lea 92(%a4),%a0
 jsr -384(%a6)
 lea 92(%a4),%a0
 jsr -372(%a6)
 move.l %d0,%d7
 /* only in the container: elsewhere trap #0 is no host call */
1: cmp.l #INPUT_MAGIC,INPUT
 bne out
 lea wbname(%pc),%a1
 moveq #37,%d0
 jsr -552(%a6)
 move.l %d0,%a5
 tst.l %d0
 beq out
 lea intuitionname(%pc),%a1
 moveq #37,%d0
 jsr -552(%a6)
 move.l %d0,%d4
 beq out
 jsr -666(%a6)
 move.l %d0,%a3
 tst.l %d0
 beq out
 /* getuid: the real uid */
 moveq #24,%d0
 trap #0
 move.l %d0,%d6
 moveq #1,%d0
 lea logoutname(%pc),%a0
 bsr additem
 tst.l %d6
 bne wait
 moveq #2,%d0
 lea haltname(%pc),%a0
 bsr additem
wait:
 move.l %a3,%a0
 jsr -384(%a6)
next:
 move.l %a3,%a0
 jsr -372(%a6)
 tst.l %d0
 beq wait
 move.l %d0,%a1
 move.l 26(%a1),%d5
 jsr -378(%a6)
 cmp.l #1,%d5
 beq logout
 cmp.l #2,%d5
 bne next
 bsr halt
 bra next
/* exit(0): the host ends the session, its display and SYS: helpers */
logout:
 clr.l -(%sp)
 clr.l -(%sp)
 moveq #1,%d0
 trap #0
 bra logout
out:
 move.l %a3,%d0
 beq 1f
 move.l %a3,%a0
 jsr -672(%a6)
1: move.l %d4,%d0
 beq 1f
 move.l %d4,%a1
 jsr -414(%a6)
1: move.l %a5,%d0
 beq 1f
 move.l %a5,%a1
 jsr -414(%a6)
1: tst.l %d7
 beq 1f
 /* Workbench unloads us once replied: no switch before we return */
 jsr -132(%a6)
 move.l %d7,%a1
 jsr -378(%a6)
1: movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
/* AddAppMenuItemA(d0 id, 0, a0 text, our port, no tags) */
additem:
 move.l %a6,-(%sp)
 moveq #0,%d1
 move.l %a3,%a1
 sub.l %a2,%a2
 move.l %a5,%a6
 jsr -72(%a6)
 move.l (%sp)+,%a6
 rts
/* ioctl(open("/dev/amiga"), AMIGAIOC_HALT): returns only when refused */
halt:
 pea 2
 pea devname(%pc)
 clr.l -(%sp)
1: moveq #5,%d0
 trap #0
 bcc 2f
 cmp.l #4,%d0
 beq 1b
 lea 12(%sp),%sp
 bra refused
2: lea 12(%sp),%sp
 move.l %d0,%d3
 clr.l -(%sp)
 move.l #AMIGAIOC_HALT,-(%sp)
 move.l %d3,-(%sp)
 clr.l -(%sp)
1: moveq #54,%d0
 trap #0
 bcc 2f
 cmp.l #4,%d0
 beq 1b
2: lea 16(%sp),%sp
 move.l %d3,-(%sp)
 clr.l -(%sp)
 moveq #6,%d0
 trap #0
 addq.l #8,%sp
/* EasyRequestArgs(no window, the note, no IDCMP, no arguments) */
refused:
 movem.l %a2-%a3/%a6,-(%sp)
 move.l %d4,%a6
 sub.l %a0,%a0
 lea easy(%pc),%a1
 sub.l %a2,%a2
 sub.l %a3,%a3
 jsr -588(%a6)
 movem.l (%sp)+,%a2-%a3/%a6
 rts
.balign 4
easy:
 .long 20, 0, easytitle, easytext, easyok
wbname: .asciz "workbench.library"
intuitionname: .asciz "intuition.library"
devname: .asciz "/dev/amiga"
logoutname: .asciz "Log Out"
haltname: .asciz "Shut Down"
easytitle: .asciz "Session"
easytext: .asciz "The machine was not shut down."
easyok: .asciz "OK"
.balign 4
