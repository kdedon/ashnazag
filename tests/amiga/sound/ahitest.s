| ahitest -- t_amiga's sound check: container.audio driven as AHI drives
| it, without AHI.  Half a second at 22050 Hz stereo; the player hook
| counts passes, the mixer writes a square wave.  The sound helper logs
| the frames, passes and kicks when play stops.
.text
.globl _start
_start:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4.w,%a6
 lea dosname(%pc),%a1
 moveq #36,%d0
 jsr -552(%a6)
 move.l %d0,%a4
 tst.l %d0
 beq 9f
 lea drvname(%pc),%a1
 moveq #4,%d0
 jsr -552(%a6)
 move.l %d0,%a5
 tst.l %d0
 beq 8f
 lea ctrl(%pc),%a2
 sub.l %a1,%a1
 exg %a5,%a6
 jsr -30(%a6)
 exg %a5,%a6
 cmp.l #14,%d0
 bne 7f
 moveq #1,%d0
 exg %a5,%a6
 jsr -54(%a6)
 exg %a5,%a6
 tst.l %d0
 bne 6f
 move.l %a6,-(%sp)
 move.l %a4,%a6
 moveq #25,%d1
 jsr -198(%a6)
 move.l (%sp)+,%a6
 moveq #1,%d0
 exg %a5,%a6
 jsr -66(%a6)
 exg %a5,%a6
6: exg %a5,%a6
 jsr -36(%a6)
 exg %a5,%a6
7: move.l %a5,%a1
 jsr -414(%a6)
8: move.l %a4,%a1
 jsr -414(%a6)
9: moveq #0,%d0
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts

player:
 addq.l #1,passes
 rts
| BuffSamples stereo frames of +-8000, the sign flipping every 50 samples
mixer:
 move.l 52(%a2),%d0
 lsl.l #1,%d0
 move.w level(%pc),%d1
 bra 2f
1: move.w %d1,(%a1)+
 subq.w #1,phase
 bne 2f
 move.w #50,phase
 neg.w %d1
2: subq.l #1,%d0
 bpl 1b
 move.w %d1,level
 rts

dosname: .asciz "dos.library"
drvname: .asciz "DEVS:AHI/container.audio"
 .asciz "$VER: ahitest 1.0 (06.10.2026)"
 .balign 4
phook: .long 0, 0, player, 0, 0
mhook: .long 0, 0, mixer, 0, 0
passes: .long 0
level: .word 8000
phase: .word 50
| struct AHIAudioCtrlDrv: stereo, 50 Hz player, 16-bit stereo buffers
ctrl:
 .long 0, 4, 0, phook
 .long 50<<16, 50<<16, 50<<16, 22050
 .word 2, 1
 .long 0, mhook, 0, 0
 .long 0, 0, 1024, 4096, 3
 .long 0, 0, 0, 0, 0
