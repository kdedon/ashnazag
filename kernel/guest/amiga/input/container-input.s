.equ BASE, 0x21000000
.equ MAGIC, 0x4d494e50
.equ head, 8
.equ tail, 12
.equ reset, 16
.equ ack, 20
.equ ready, 24
.equ events, 32
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 moveq #20,%d7
 move.l #BASE,%a5
 cmp.l #MAGIC,(%a5)
 bne done
 cmp.l #1,4(%a5)
 bne done
 move.l 4,%a6
 jsr -132(%a6)
 tst.l ready(%a5)
 bne occupied
 move.l #2,ready(%a5)
 jsr -138(%a6)
 sub.l %a2,%a2
 sub.l %a3,%a3
 sub.l %a4,%a4
 lea dosname(%pc),%a1
 moveq #37,%d0
 jsr -552(%a6)
 move.l %d0,%a4
 tst.l %d0
 beq cleanup
 jsr -666(%a6)
 move.l %d0,%a3
 tst.l %d0
 beq cleanup
 move.l %a3,%a0
 moveq #48,%d0
 jsr -654(%a6)
 move.l %d0,%a2
 tst.l %d0
 beq cleanup
 lea inputname(%pc),%a0
 moveq #0,%d0
 moveq #0,%d1
 move.l %a2,%a1
 jsr -444(%a6)
 tst.l %d0
 bne cleanup
 move.l head(%a5),tail(%a5)
 move.l reset(%a5),ack(%a5)
 addq.l #1,28(%a5)
 move.l #1,ready(%a5)
loop:
 moveq #0,%d0
 moveq #0,%d1
 jsr -306(%a6)
 btst #12,%d0
 bne interrupted
 move.l reset(%a5),%d3
 cmp.l ack(%a5),%d3
 beq drain
 bsr release_all
 move.l head(%a5),tail(%a5)
 move.l %d3,ack(%a5)
drain:
 moveq #63,%d6
next:
 move.l reset(%a5),%d0
 cmp.l ack(%a5),%d0
 bne loop
 move.l tail(%a5),%d2
 move.l head(%a5),%d0
 sub.l %d2,%d0
 beq sleep
 cmp.l #256,%d0
 bhi stop
 move.l %d2,%d0
 and.l #255,%d0
 mulu.w #12,%d0
 lea events(%a5,%d0.l),%a0
 lea event(%pc),%a1
 move.b 1(%a0),4(%a1)
 clr.b 5(%a1)
 move.w 2(%a0),6(%a1)
 move.w 4(%a0),8(%a1)
 move.l 8(%a0),10(%a1)
 move.l reset(%a5),%d0
 cmp.l ack(%a5),%d0
 bne loop
 addq.l #1,%d2
 move.l %d2,tail(%a5)
 lea event(%pc),%a0
 moveq #0,%d0
 move.w 6(%a0),%d0
 move.l %d0,%d1
 and.l #127,%d0
 cmp.b #1,4(%a0)
 beq track
 cmp.b #2,4(%a0)
 bne tracked
 cmp.l #0x68,%d0
 blo tracked
 cmp.l #0x6a,%d0
 bhi tracked
track:
 cmp.l #127,%d0
 bhi tracked
 lea held(%pc),%a0
 btst #7,%d1
 seq (%a0,%d0.l)
tracked:
 bsr send
 tst.l %d0
 bne stop
 dbra %d6,next
sleep:
 move.l %a6,-(%sp)
 move.l %a4,%a6
 moveq #1,%d1
 jsr -198(%a6)
 move.l (%sp)+,%a6
 bra loop
interrupted:
 moveq #0,%d7
stop:
 clr.l ready(%a5)
 bsr release_all
 move.l %a2,%a1
 jsr -450(%a6)
cleanup:
 clr.l ready(%a5)
 cmp.l #0,%a2
 beq 1f
 move.l %a2,%a0
 jsr -660(%a6)
1: cmp.l #0,%a3
 beq 2f
 move.l %a3,%a0
 jsr -672(%a6)
2: cmp.l #0,%a4
 beq done
 move.l %a4,%a1
 jsr -414(%a6)
 bra done
occupied:
 jsr -138(%a6)
done:
 move.l %d7,%d0
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts
send:
 move.w #11,28(%a2)
 clr.b 30(%a2)
 move.l #22,36(%a2)
 lea event(%pc),%a0
 move.l %a0,40(%a2)
 move.l %a2,%a1
 jsr -456(%a6)
 rts
release_all:
 movem.l %d2/%a0,-(%sp)
 moveq #0,%d2
1: lea held(%pc),%a0
 tst.b (%a0,%d2.l)
 beq 3f
 clr.b (%a0,%d2.l)
 lea event(%pc),%a0
 clr.b 5(%a0)
 clr.w 8(%a0)
 clr.l 10(%a0)
 move.b #1,4(%a0)
 cmp.w #0x68,%d2
 blo 2f
 move.b #2,4(%a0)
 move.w #0x8000,8(%a0)
2: move.w %d2,%d0
 or.w #0x80,%d0
 move.w %d0,6(%a0)
 bsr send
3: addq.w #1,%d2
 cmp.w #128,%d2
 blo 1b
 movem.l (%sp)+,%d2/%a0
 rts
inputname: .asciz "input.device"
dosname: .asciz "dos.library"
 .asciz "$VER: container-input 1.0 (04.10.2026)"
 .balign 2
event: .space 22
held: .space 128
