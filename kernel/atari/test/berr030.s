| A 68030 data-read bus error completed by the handler, as the manual
| says: data in the long frame's input buffer (+$2C), DF cleared.  The
| read must not be rerun.  Runs from AUTO under TOS 4.04; reports
| through NF_STDERR and exits with NF_SHUTDOWN.
	.text
start:	clr.l	-(%sp)
	move.w	#0x20,-(%sp)		| Super(0)
	trap	#1
	addq.l	#6,%sp
	ori.w	#0x0700,%sr
	lea	vars(%pc),%a5
	lea	nfstderr(%pc),%a0
	bsr	nfid
	move.l	%d0,ID_ERR(%a5)
	lea	nfshut(%pc),%a0
	bsr	nfid
	move.l	%d0,ID_SHUT(%a5)
	lea	berr(%pc),%a0
	move.l	%a0,0x8.w
	lea	totop(%pc),%a0
	move.l	%a0,0x80.w		| trap #0: back to supervisor
	| root table: 16 x 256 MB pages, the top one invalid
	lea	root+15(%pc),%a0
	move.l	%a0,%d0
	andi.w	#0xfff0,%d0
	move.l	%d0,%a0
	move.l	%d0,CRP+4(%a5)
	move.l	#0x7fff0002,CRP(%a5)
	moveq	#14,%d1
	move.l	#1,%d2
1:	move.l	%d2,(%a0)+
	addi.l	#0x10000000,%d2
	dbra	%d1,1b
	clr.l	(%a0)
	moveq	#'0',%d7
	move.l	#0x80f04d00,TC(%a5)
	pflusha
	pmove	CRP(%a5),%crp
	pmove	TC(%a5),%tc

	| t1: move.w $8006.w,d0  (as TOS 4.04 does at $E00034)
	bsr	reset
	moveq	#0,%d0
	.word	0x3038,0x8006
	move.l	%d0,%d6
	sub.l	%a6,%a6
	bsr	report

	| t2: move.w (a0)+,d1
	bsr	reset
	move.l	#0xffff8006,%a0
	moveq	#0,%d1
	move.w	(%a0)+,%d1
	move.l	%d1,%d6
	move.l	%a0,%a6
	bsr	report

	| t3: move.w (a0)+,(a1)+  (the palette copy)
	bsr	reset
	move.l	#0xffff8006,%a0
	lea	buf(%pc),%a1
	clr.l	(%a1)
	move.w	(%a0)+,(%a1)+
	move.l	%a0,%a6
	lea	buf(%pc),%a2
	move.l	%a1,%d6
	sub.l	%a2,%d6			| a1 advance: 2
	swap	%d6
	move.w	(%a2),%d6
	bsr	report

	| t4: t2 in user mode
	bsr	reset
	lea	ustack(%pc),%a0
	move.l	%a0,%usp
	andi.w	#0xdfff,%sr
	move.l	#0xffff8006,%a0
	moveq	#0,%d1
	move.w	(%a0)+,%d1
	trap	#0
	move.l	%d1,%d6
	move.l	%a0,%a6
	bsr	report

done:	clr.l	TC(%a5)
	pmove	TC(%a5),%tc
	move.l	ID_SHUT(%a5),-(%sp)
	bsr	nfcall
9:	bra	9b

reset:	addq.b	#1,%d7
	clr.l	CNT(%a5)
	clr.l	FMT(%a5)
	clr.l	FPC(%a5)
	rts

totop:	ori.w	#0x2000,(%sp)
	rte

berr:	move.l	%a5,-(%sp)
	lea	vars(%pc),%a5
	addq.l	#1,CNT(%a5)
	move.w	4+6(%sp),FMT(%a5)
	move.w	4+10(%sp),SSW(%a5)
	move.l	4+2(%sp),FPC(%a5)
	cmpi.l	#4,CNT(%a5)
	bhi.s	1f
	move.l	#0x1234,4+0x2c(%sp)
	andi.w	#0xfeff,4+10(%sp)
	move.l	(%sp)+,%a5
	rte
1:	moveq	#-1,%d6			| rerun after rte: report and stop
	sub.l	%a6,%a6
	bsr	report
	bra	done

| "tN n=COUNT d=D6 a=A6 f=FMT/SSW"
report:	lea	line(%pc),%a0
	move.b	%d7,1(%a0)
	lea	5(%a0),%a0
	move.l	CNT(%a5),%d0
	bsr	hex
	addq.l	#3,%a0
	move.l	%d6,%d0
	bsr	hex
	addq.l	#3,%a0
	move.l	%a6,%d0
	bsr	hex
	addq.l	#3,%a0
	move.l	FMT(%a5),%d0
	bsr	hex
	addq.l	#3,%a0
	move.l	FPC(%a5),%d0
	bsr	hex
	addq.l	#3,%a0
	move.l	(%sp),%d0
	bsr	hex
	pea	line(%pc)
	move.l	ID_ERR(%a5),-(%sp)
	bsr	nfcall
	addq.l	#8,%sp
	rts

hex:	moveq	#7,%d1
1:	rol.l	#4,%d0
	move.b	%d0,%d2
	andi.w	#15,%d2
	move.b	digits(%pc,%d2.w),(%a0)+
	dbra	%d1,1b
	rts
digits:	.ascii	"0123456789abcdef"

nfid:	move.l	%a0,-(%sp)
	bsr.s	1f
	addq.l	#4,%sp
	rts
1:	.word	0x7300
	rts
nfcall:	.word	0x7301
	rts

	.equ	ID_ERR,0
	.equ	ID_SHUT,4
	.equ	CNT,8
	.equ	FMT,12
	.equ	SSW,14
	.equ	TC,16
	.equ	CRP,20
	.equ	FPC,28
	.even
vars:	.space	32
buf:	.space	4
line:	.ascii	"t? n=xxxxxxxx d=xxxxxxxx a=xxxxxxxx f=xxxxxxxx p=xxxxxxxx r=xxxxxxxx\n\0"
nfstderr: .asciz "NF_STDERR"
nfshut:	.asciz	"NF_SHUTDOWN"
	.even
root:	.space	96
	.space	512
ustack:	.long	0
