| timetest -- t_amiga's timer check: a one-second MICROHZ request and
| Delay(50), each timed by ReadEClock, and E-clock steps back over 20000
| reads.  SYS: is read-only, so each result goes out as the name of a
| file it fails to open, which the session log shows.
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4.w,%a6
 lea dosname(%pc),%a1
 moveq #39,%d0
 jsr -552(%a6)
 tst.l %d0
 beq 9f
 move.l %d0,%a5
 jsr -666(%a6)
 tst.l %d0
 beq 8f
 move.l %d0,%a4
 move.l %d0,%a0
 moveq #40,%d0
 jsr -654(%a6)
 tst.l %d0
 beq 7f
 move.l %d0,%a3
 lea timername(%pc),%a0
 move.l %a3,%a1
 moveq #0,%d0
 moveq #0,%d1
 jsr -444(%a6)
 tst.b %d0
 bne 6f
 moveq #0,%d0
 bsr put
 lea vals(%pc),%a2
 bsr eclock
 move.l %d0,(%a2)
 moveq #1,%d0
 bsr put
 bsr eclock
 move.l %d1,%d4
 move.w #9,28(%a3)
 move.l #1,32(%a3)
 clr.l 36(%a3)
 move.l %a3,%a1
 jsr -456(%a6)
 bsr eclock
 sub.l %d4,%d1
 move.l %d1,4(%a2)
 moveq #2,%d0
 bsr put
 bsr eclock
 move.l %d1,%d5
 exg %a5,%a6
 moveq #50,%d1
 jsr -198(%a6)
 exg %a5,%a6
 bsr eclock
 sub.l %d5,%d1
 move.l %d1,8(%a2)
 moveq #3,%d0
 bsr put
| d6:d7 the last value read
 moveq #0,%d4
 move.l #20000,%d5
 bsr eclock
 move.l %a0,%d6
 move.l %d1,%d7
1: bsr eclock
 move.l %a0,%d2
 cmp.l %d6,%d2
 bhi 3f
 bcs 2f
 cmp.l %d7,%d1
 bcc 3f
2: addq.l #1,%d4
3: move.l %d2,%d6
 move.l %d1,%d7
 subq.l #1,%d5
 bne 1b
 move.l %d4,12(%a2)
 moveq #4,%d0
 bsr put
 move.l %a3,%a1
 jsr -450(%a6)
6: move.l %a3,%a0
 jsr -660(%a6)
7: move.l %a4,%a0
 jsr -672(%a6)
8: move.l %a5,%a1
 jsr -414(%a6)
9: movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
| the d0th name, with value d0-1, opened and closed
put:
 movem.l %d2/%a2-%a3/%a6,-(%sp)
 lea fmts(%pc),%a0
 move.w %d0,%d1
 mulu #24,%d1
 add.l %d1,%a0
 lea vals-4(%pc),%a1
 lsl.w #2,%d0
 add.w %d0,%a1
 lea putch(%pc),%a2
 lea name(%pc),%a3
 move.l 4.w,%a6
 jsr -522(%a6)
 move.l %a5,%a6
 lea name(%pc),%a0
 move.l %a0,%d1
 move.l #1005,%d2
 jsr -30(%a6)
 move.l %d0,%d1
 beq 1f
 jsr -36(%a6)
1: movem.l (%sp)+,%d2/%a2-%a3/%a6
 rts
putch:
 move.b %d0,(%a3)+
 rts
| ReadEClock: d0 the frequency, a0:d1 the count
eclock:
 move.l %a6,-(%sp)
 move.l 20(%a3),%a6
 lea ev(%pc),%a0
 jsr -60(%a6)
 move.l (%sp)+,%a6
 lea ev(%pc),%a0
 move.l 4(%a0),%d1
 move.l (%a0),%a0
 rts
.balign 4
ev: .long 0, 0
vals: .long 0, 0, 0, 0
dosname: .asciz "dos.library"
timername: .asciz "timer.device"
name: .space 64
fmts: .ascii "SYS:timeres start\0\0\0\0\0\0\0"
 .ascii "SYS:timeres freq %ld\0\0\0\0"
 .ascii "SYS:timeres micro %ld\0\0\0"
 .ascii "SYS:timeres delay %ld\0\0\0"
 .ascii "SYS:timeres back %ld\0\0\0\0"
.balign 2
