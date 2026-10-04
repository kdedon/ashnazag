| irq.s -- interrupt priority and masking as a TOS program sees them.
|
| long irqtest(int k): 0 when it behaves, else a failure code.  With the
| IPL at 7 until timer C (MFP level 6) is pending, and so the VBL (level
| 4) from the same tick, then:
|   1  rte to IPL 3: timer C at the rte's target PC, then the VBL
|   2  move #$2300,sr: the same at the next instruction
|   3  IPL 5: timer C only, through SR moves that keep 5 or 4; then
|      IPL 3 lets the VBL in
|   4  IPL 3, timer C's handler drops to 3: the VBL nests inside it
| Each entry is logged: level, the frame's SR and PC, the SR inside.

NLOG = 64

	.text
	.globl	irqtest
irqtest:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),d7
	clr.l	-(sp)			| Super(0)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,a6
	move.w	sr,d6
	move.w	#0x2700,sr
	move.l	0x70,oldvbl
	move.l	0x114,oldtc
	move.l	#myvbl,0x70
	move.l	#mytc,0x114
	clr.w	nest
	bsr	waittc
	move.l	#1,d0
	tst.l	d5
	beq	out			| timer C never pending
	clr.l	nlog
	cmp.l	#1,d7
	beq	k1
	cmp.l	#2,d7
	beq	k2
	cmp.l	#3,d7
	beq	k3
	bra	k4

k1:	move.w	#0,-(sp)		| format 0
	pea	k1lab(pc)
	move.w	#0x2300,-(sp)
	rte
k1lab:	lea	k1lab(pc),a0
	move.l	a0,d2
	bsr	both
	bra	out

k2:	move.w	#0x2300,sr
k2lab:	lea	k2lab(pc),a0
	move.l	a0,d2
	bsr	both
	bra	out

k3:	move.w	#0x2500,sr
	move.l	#50,d3
1:	move.w	sr,d0			| SR moves that keep the VBL masked
	move.w	d0,sr
	move.w	sr,-(sp)
	move.w	(sp)+,sr
	ori.w	#0x0100,sr
	andi.w	#0xfeff,sr
	move.w	#0x2400,sr
	move.w	#0x2500,sr
	subq.l	#1,d3
	bne	1b
	move.l	#2,d0
	bsr	count			| d1: VBL entries
	tst.l	d1
	bne	out
	move.l	#3,d0
	bsr	count
	tst.l	d4			| timer C taken
	beq	out
	move.w	#0x2300,sr
	move.l	#4,d0
	bsr	count
	tst.l	d1
	beq	out
	bsr	sane
	bra	out

k4:	move.w	#1,nest
	move.w	#0x2300,sr
k4lab:	clr.w	nest
	move.l	#nestpc,d2
	bsr	vblat
	tst.l	d0
	bne	out
	bsr	sane
	bra	out

| d2: the PC timer C came back to, first; a VBL after it
both:	bsr	sane
	tst.l	d0
	bne	9f
	move.l	#4,d0
	tst.l	nlog
	beq	9f
	lea	log,a0
	cmp.w	#6,(a0)
	bne	9f
	cmp.l	4(a0),d2
	bne	9f
	move.l	#5,d0
	bsr	count
	tst.l	d1
	beq	9f
	moveq	#0,d0
9:	rts

| a VBL entry whose frame PC is d2, from IPL 3
vblat:	move.l	#7,d0
	lea	log,a0
	bsr	logged
1:	tst.l	d3
	beq	9f
	cmp.w	#4,(a0)
	bne	2f
	cmp.l	4(a0),d2
	bne	2f
	move.w	2(a0),d1
	and.w	#0x0700,d1
	cmp.w	#0x0300,d1
	beq	3f
2:	lea	12(a0),a0
	subq.l	#1,d3
	bra	1b
3:	moveq	#0,d0
9:	rts

| entries: d1 VBL, d4 timer C
count:	moveq	#0,d1
	moveq	#0,d4
	lea	log,a0
	bsr	logged
1:	tst.l	d3
	beq	9f
	cmp.w	#4,(a0)
	bne	2f
	addq.l	#1,d1
	bra	3f
2:	addq.l	#1,d4
3:	lea	12(a0),a0
	subq.l	#1,d3
	bra	1b
9:	rts

| every entry: level above the frame's IPL, and the IPL inside = level
sane:	lea	log,a0
	bsr	logged
	move.l	#8,d0
1:	tst.l	d3
	beq	2f
	move.w	(a0),d1
	lsl.w	#8,d1
	move.w	2(a0),d4
	and.w	#0x0700,d4
	cmp.w	d1,d4
	bcc	9f			| delivered while masked
	move.w	8(a0),d4
	and.w	#0x0700,d4
	cmp.w	d1,d4
	bne	9f
	lea	12(a0),a0
	subq.l	#1,d3
	bra	1b
2:	moveq	#0,d0
9:	rts

| d3: the entries in the log
logged:	move.l	nlog,d3
	cmp.l	#NLOG,d3
	bls	9f
	move.l	#NLOG,d3
9:	rts

| d5: 0 when timer C never became pending
waittc:	move.l	#200000,d5
	lea	0xfffffa0d,a1		| IPRB
1:	btst	#5,(a1)
	bne	9f
	subq.l	#1,d5
	bne	1b
9:	rts

out:	move.w	#0x2700,sr
	move.l	oldvbl,0x70
	move.l	oldtc,0x114
	move.w	d6,sr
	move.l	d0,d7
	move.l	a6,-(sp)		| Super(old)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d7,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts

| from a handler, below 12 bytes of registers: the SR inside at 16(sp),
| the frame's SR at 18(sp) and PC at 20(sp)
logit:	move.l	nlog,d1
	cmp.l	#NLOG,d1
	bcc	9f
	mulu.w	#12,d1
	lea	log,a0
	add.l	d1,a0
	move.w	d0,(a0)
	move.w	18(sp),2(a0)
	move.l	20(sp),4(a0)
	move.w	16(sp),8(a0)
9:	addq.l	#1,nlog
	rts

myvbl:	move.w	sr,-(sp)
	movem.l	d0-d1/a0,-(sp)
	moveq	#4,d0
	bsr	logit
	movem.l	(sp)+,d0-d1/a0
	addq.l	#2,sp
	move.l	oldvbl,-(sp)
	rts

mytc:	move.w	sr,-(sp)
	movem.l	d0-d1/a0,-(sp)
	moveq	#6,d0
	bsr	logit
	tst.w	nest
	beq	1f
	clr.w	nest
	move.w	#0x2300,sr
nestpc:	nop
	move.w	#0x2600,sr
1:	movem.l	(sp)+,d0-d1/a0
	addq.l	#2,sp
	move.l	oldtc,-(sp)
	rts

	.bss
	.lcomm	oldvbl, 4
	.lcomm	oldtc, 4
	.lcomm	nlog, 4
	.lcomm	nest, 2
	.lcomm	log, NLOG * 12
