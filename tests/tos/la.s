| la.s -- Line-A and XBIOS calls for latest.c
	.text
	.globl	lacall, lafonts, logbase, hz200, setscreen
| long lacall(n, a6, d0, d1, a0, a2): Line-A $A00n with those registers; d0 back
lacall:	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),d2
	move.l	52(sp),a6
	move.l	56(sp),d0
	move.l	60(sp),d1
	move.l	64(sp),a0
	move.l	68(sp),a2
	lea	tab,a1
	jsr	(a1,d2.l*4)
	tst.l	48(sp)
	bne.s	1f
	move.l	a1,lafonts
	move.l	a0,d0
1:	movem.l	(sp)+,d2-d7/a2-a6
	rts
tab:	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.word	0xa000+\n
	rts
	.endr
logbase: move.w	#3,-(sp)
	trap	#14
	addq.l	#2,sp
	rts
| setscreen(log, phys): XBIOS Setscreen, resolution unchanged
setscreen: move.w	#-1,-(sp)
	move.l	10(sp),-(sp)
	move.l	10(sp),-(sp)
	move.w	#5,-(sp)
	trap	#14
	lea	12(sp),sp
	rts
| the 200 Hz counter
hz200:	pea	1f
	move.w	#38,-(sp)
	trap	#14
	addq.l	#6,sp
	rts
1:	move.l	0x4ba,d0
	rts
	.data
lafonts: .long	0
