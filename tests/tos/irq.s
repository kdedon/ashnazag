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
|   5  timer C's handler drops to 3 for longer than a tick, three
|      times: timer C stays in service, its handler is not entered again
|   6  timers A and B at 1 kHz (A's handler lowers the IPL, so it
|      nests), while SR writes, rtes and Jdisint/Jenabint of both run
|      in a loop, half a second in supervisor mode, then in user mode:
|      registers and the stack survive (11 supervisor, 12 user), both
|      timers ran (13)
|   7  the timers of 6, while user mode calls Supexec and Super(0)
|      and back in a loop; the Supexec'd function writes SR: registers,
|      the stack and its result survive (14), both timers ran (13)
|   8  the timers of 6, while user mode starts a child 200 times
|      (Pexec 5, then 6); the child calls Supexec and exits with 7 (16)
| Each entry is logged: level, the frame's SR and PC, the SR inside.

NLOG = 64
USTK = 4096
CHMEM = 4096

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
	bsr	calib
	move.w	#0x2700,sr
	move.l	0x70,oldvbl
	move.l	0x114,oldtc
	move.l	#myvbl,0x70
	move.l	#mytc,0x114
	clr.w	nest
	bsr	waittc
	clr.l	nlog
	cmp.l	#1,d7
	beq	k1
	cmp.l	#2,d7
	beq	k2
	cmp.l	#3,d7
	beq	k3
	cmp.l	#4,d7
	beq	k4
	cmp.l	#5,d7
	beq	k5
	cmp.l	#6,d7
	beq	k6
	cmp.l	#7,d7
	beq	k7
	bra	k8

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

k5:	clr.w	depth
	clr.w	nested
	move.w	#3,slow
	move.l	0x466,d3		| 100 VBLs for the three
	add.l	#100,d3
	move.w	#0x2300,sr
1:	tst.w	slow
	beq	2f
	cmp.l	0x466,d3
	bhi	1b
	moveq	#9,d0			| the handler never ran three times
	bra	out
2:	moveq	#10,d0
	tst.w	nested
	bne	out
	moveq	#0,d0
	bra	out

k6:	bsr	tstart
	lea	srs(pc),a5
	move.w	#0x2300,sr
	move.l	0x4ba,d3
	add.l	#100,d3
	moveq	#1,d5
	moveq	#11,d0
	bsr	srloop
	bne	6f
	bsr	touser
	moveq	#0,d5
	move.l	#20000,d3
	moveq	#12,d0
	bsr	srloop
	move.l	d0,d4
	clr.l	-(sp)			| Super(0)
	move.w	#0x20,-(sp)
	trap	#1
	move.l	ssp0,sp
	move.l	usp0,a0
	move.l	a0,usp
	move.l	d4,d0
	bne	6f
	moveq	#13,d0
	tst.l	acount
	beq	6f
	tst.l	bcount
	beq	6f
	moveq	#0,d0
6:	move.l	d0,d4
	bsr	tstop
	move.l	d4,d0
	bra	out

k7:	bsr	tstart
	bsr	touser
	move.l	#0x66666666,d5
	move.l	#0x77777777,a5
	move.l	#4000,d3
	moveq	#14,d4
1:	pea	sfn(pc)
	move.w	#38,-(sp)		| Supexec
	trap	#14
	addq.l	#6,sp
	cmp.l	#0x5a5aa5a5,d0
	bne	kend
	moveq	#15,d0
	and.l	d3,d0
	bne	2f
	clr.l	-(sp)			| Super(0): the user stack becomes the supervisor's
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,a3
	move.w	#0x2700,sr
	move.w	#0x2300,sr
	move.l	a3,-(sp)		| Super(old): back to user mode
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
2:	cmp.l	sp,a4
	bne	kend
	cmp.l	#0x66666666,d5
	bne	kend
	cmp.l	#0x77777777,a5
	bne	kend
	subq.l	#1,d3
	beq	3f
	move.l	acount,d0
	cmp.l	adead,d0
	bcs	1b
3:	moveq	#0,d4
kend:	clr.l	-(sp)			| Super(0)
	move.w	#0x20,-(sp)
	trap	#1
	move.l	ssp0,sp
	move.l	usp0,a0
	move.l	a0,usp
	move.l	d4,d0
	bne	6b
	moveq	#13,d0
	tst.l	acount
	beq	6b
	tst.l	bcount
	beq	6b
	moveq	#0,d0
	bra	6b

k8:	bsr	tstart
	bsr	touser
	move.l	#200,d3
	moveq	#16,d4
1:	clr.l	-(sp)			| the parent's environment
	pea	notail(pc)
	clr.l	-(sp)
	move.l	#0x004b0005,-(sp)	| Pexec 5: a basepage
	trap	#1
	lea	16(sp),sp
	tst.l	d0
	ble	kend
	move.l	d0,a3
	pea	CHMEM
	move.l	a3,-(sp)
	move.l	#0x004a0000,-(sp)	| Mshrink
	trap	#1
	lea	12(sp),sp
	lea	CHMEM(a3),a0
	move.l	a0,4(a3)		| p_hitpa: the child's stack
	lea	child(pc),a0
	move.l	a0,8(a3)		| p_tbase
	clr.l	-(sp)
	move.l	a3,-(sp)
	clr.l	-(sp)
	move.l	#0x004b0006,-(sp)	| Pexec 6: run it, then free it
	trap	#1
	lea	16(sp),sp
	cmp.l	#7,d0
	bne	kend
	cmp.l	sp,a4
	bne	kend
	subq.l	#1,d3
	beq	3f
	move.l	acount,d0
	cmp.l	adead,d0
	bcs	1b
3:	moveq	#0,d4
	bra	kend

child:	pea	sfn(pc)
	move.w	#38,-(sp)		| Supexec
	trap	#14
	addq.l	#6,sp
	moveq	#7,d1
	cmp.l	#0x5a5aa5a5,d0
	beq	1f
	moveq	#99,d1
1:	move.w	d1,-(sp)
	move.w	#0x4c,-(sp)		| Pterm
	trap	#1

notail:	.word	0

| from supervisor mode to user mode on ustk at IPL 3; a4 the user sp;
| user mode loops end at their count or after 2000 timer A interrupts
touser:	move.l	(sp)+,a1
	move.l	acount,d0
	add.l	#2000,d0
	move.l	d0,adead
	move.w	#0x2300,sr
	move.l	usp,a0			| Super(old) returns on it
	move.l	a0,usp0
	lea	ustk+USTK,a0
	move.l	a0,usp
	move.l	sp,ssp0
	andi.w	#0xdfff,sr
	move.l	sp,a4
	jmp	(a1)

| called by Supexec: supervisor mode, SR writes; 5a5aa5a5 if so
sfn:	move.w	sr,d0
	btst	#13,d0
	beq	1f
	move.w	#0x2700,sr
	move.w	#0x2300,sr
	moveq	#20,d0
2:	dbra	d0,2b
	ori.w	#0x0700,sr
	andi.w	#0xfcff,sr
	move.w	#0x2300,sr
	move.l	#0x5a5aa5a5,d0
1:	rts

| timers A and B at 997 Hz, mya and myb
tstart:	move.l	0x134,olda
	move.l	0x120,oldb
	clr.l	acount
	clr.l	bcount
	clr.w	adepth
	pea	mya(pc)
	move.w	#154,-(sp)		| timer A, /16, 154: 997 Hz
	move.l	#3,-(sp)
	move.w	#31,-(sp)		| Xbtimer
	trap	#14
	lea	12(sp),sp
	pea	myb(pc)
	move.w	#154,-(sp)		| timer B the same
	move.l	#0x00010003,-(sp)
	move.w	#31,-(sp)
	trap	#14
	lea	12(sp),sp
	rts

tstop:	move.w	#0x2700,sr
	moveq	#0,d1
	bsr	xbt
	moveq	#1,d1
	bsr	xbt
	move.l	olda,0x134
	move.l	oldb,0x120
	rts

| timer d1 stopped
xbt:	clr.l	-(sp)
	clr.l	-(sp)
	move.w	d1,-(sp)
	move.w	#31,-(sp)
	trap	#14
	lea	12(sp),sp
	rts

| Jdisint or Jenabint (d1: 26, 27) of timer A (13), then of B (8)
jint:	move.w	#8,-(sp)
	move.w	d1,-(sp)
	move.w	#13,-(sp)
	move.w	d1,-(sp)
	trap	#14
	addq.l	#4,sp
	trap	#14
	addq.l	#4,sp
	rts

| d0 the failure code, d5 nonzero in supervisor mode: SR writes, rtes
| and every 64 loops Jdisint/Jenabint of both timers, until _hz_200
| reaches d3, or d3 loops in user mode; registers and sp checked each
| time: 0 or d0, flags set
srloop:	move.l	d0,d7
	move.l	sp,a4
1:	addq.l	#1,nloop
	move.l	nloop,d0
	and.w	#63,d0
	bne	5f
	movem.l	d0-d2/a0-a2,-(sp)	| the XBIOS keeps d3-d7/a3-a6
	moveq	#26,d1
	bsr	jint
	moveq	#27,d1
	bsr	jint
	movem.l	(sp)+,d0-d2/a0-a2
5:	move.l	#0x11111111,d0
	move.l	#0x22222222,d1
	move.l	#0x33333333,d2
	move.l	#0x44444444,a0
	move.l	#0x55555555,a1
	tst.l	d5
	beq	3f
	move.w	#0x2700,sr
	move.w	#0x2300,sr
	move.w	6(a5),sr		| 2700
	move.w	4(a5),sr		| 2300
	move.w	#0x2700,sr
	move.w	(a5),sr			| 2600
	andi.w	#0xf8ff,sr
	ori.w	#0x0300,sr
	move.w	#0x2700,-(sp)
	move.w	(sp)+,sr
	move.w	4(a5),d4
	move.w	d4,sr
	move.w	#0x2700,sr
	clr.w	-(sp)			| format 0
	pea	2f(pc)
	move.w	#0x2300,-(sp)
	rte
2:	bra	4f
3:	moveq	#9,d4			| user: interrupts return here through rte
6:	dbra	d4,6b
	subq.l	#1,d3
4:	cmp.l	sp,a4
	bne	9f
	cmp.l	#0x11111111,d0
	bne	9f
	cmp.l	#0x22222222,d1
	bne	9f
	cmp.l	#0x33333333,d2
	bne	9f
	cmp.l	#0x44444444,a0
	bne	9f
	cmp.l	#0x55555555,a1
	bne	9f
	tst.l	d5
	beq	7f
	cmp.l	0x4ba,d3
	bhi	1b
	bra	8f
7:	tst.l	d3
	beq	8f
	move.l	acount,d0
	cmp.l	adead,d0
	bcs	1b
8:	moveq	#0,d0
	rts
9:	move.l	d7,d0
	rts

| timer A: lowers the IPL to 5, so it nests, four deep at most
mya:	movem.l	d0/a0,-(sp)
	addq.l	#1,acount
	lea	srs(pc),a0
	addq.w	#1,adepth
	cmp.w	#4,adepth
	bcc	1f
	move.w	2(a0),sr		| 2500
	moveq	#20,d0
2:	dbra	d0,2b
	move.w	(a0),sr			| 2600
1:	subq.w	#1,adepth
	movem.l	(sp)+,d0/a0
	rte

| timer B: SR moves at its own level
myb:	move.l	a0,-(sp)
	addq.l	#1,bcount
	lea	srs(pc),a0
	move.w	(a0),sr			| 2600
	move.w	sr,-(sp)
	move.w	(sp)+,sr
	move.l	(sp)+,a0
	rte

srs:	.word	0x2600, 0x2500, 0x2300, 0x2700

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
| d5: four times the loops of waittc's body in 5 ticks of _hz_200
calib:	move.l	0x4ba,d0
1:	cmp.l	0x4ba,d0
	beq	1b
	move.l	0x4ba,d0
	addq.l	#5,d0
	moveq	#0,d5
2:	addq.l	#1,d5
	cmp.l	0x4ba,d0
	bhi	2b
	lsl.l	#2,d5
	rts

| 100 ms with the IPL at 7: timer C and the VBL are pending after it
waittc:	cmp.l	0x4ba,d0
	subq.l	#1,d5
	bne	waittc
	rts

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
	tst.w	slow
	beq	3f
	addq.w	#1,depth
	cmp.w	#1,depth
	beq	4f
	move.w	#1,nested		| entered again while in service
	bra	5f
4:	move.w	#0x2300,sr		| two VBLs, some timer C periods
	move.l	0x466,d0
	addq.l	#2,d0
6:	cmp.l	0x466,d0
	bhi	6b
	move.w	#0x2600,sr
	subq.w	#1,slow
5:	subq.w	#1,depth
3:	tst.w	nest
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
	.lcomm	slow, 2
	.lcomm	depth, 2
	.lcomm	nested, 2
	.lcomm	log, NLOG * 12
	.lcomm	olda, 4
	.lcomm	ssp0, 4
	.lcomm	usp0, 4
	.lcomm	acount, 4
	.lcomm	bcount, 4
	.lcomm	oldb, 4
	.lcomm	nloop, 4
	.lcomm	adead, 4
	.lcomm	adepth, 2
	.lcomm	ustk, USTK

| long traptest9(), traptest10(), traptest0(): 0 when a handler at the vector
| of trap #9 (#10, #0) gets the call: d0 + 1 comes back.
	.text
	.macro	tt name, vec, n
	.globl	\name
\name:
	movem.l	d2-d7/a2-a6,-(sp)
	clr.l	-(sp)			| Super(0)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d0,a6
	move.l	\vec,a5		| the vector, for the restore
	move.l	#th,\vec
	moveq	#5,d0
	trap	#\n
	moveq	#1,d6
	cmp.l	#6,d0
	bne	1f
	moveq	#0,d6
1:	move.l	a5,\vec
	move.l	a6,-(sp)		| Super(old)
	move.w	#0x20,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	d6,d0
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	.endm

	tt	traptest9, 0xa4, 9
	tt	traptest0, 0x80, 0
	tt	traptest10, 0xa8, 10

th:	addq.l	#1,d0
	rte
