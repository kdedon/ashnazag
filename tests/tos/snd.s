| snd.s -- for utest's sound check: XBIOS calls, Timer A's handler,
| and the cookie and clock as Supexec reads them.
	.text
	.globl	trap14, sndend, sndends, supcookie, suphz

| long trap14(short *w, int n): the XBIOS call whose n words are w
trap14:	movem.l	d2/a2-a3,-(sp)
	move.l	sp,a3
	move.l	16(sp),a0
	move.l	20(sp),d1
	lea	(a0,d1.l*2),a0
1:	move.w	-(a0),-(sp)
	subq.l	#1,d1
	bne.s	1b
	trap	#14
	move.l	a3,sp
	movem.l	(sp)+,d2/a2-a3
	rts

| Timer A, counting buffer ends
sndend:	addq.l	#1,sndends
	bclr	#5,0xfffa0f		| ISRA: end of service
	rte

| _SND, -1 without one
supcookie:
	move.l	0x5a0,d0
	beq.s	3f
	move.l	d0,a0
1:	move.l	(a0),d0
	beq.s	3f
	cmp.l	#0x5f534e44,d0
	beq.s	2f
	addq.l	#8,a0
	bra.s	1b
2:	move.l	4(a0),d0
	rts
3:	moveq	#-1,d0
	rts

suphz:	move.l	0x4ba,d0
	rts

	.data
	.balign	4
sndends: .long	0
