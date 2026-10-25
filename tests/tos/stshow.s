| stshow.s -- STSHOW.TOS: a text screen, then a screen of its own.
| Fills the console with reversed spaces, makes C:\STSHOW.1 and waits for
| a key; sets its own screen at the same resolution (Setscreen with the
| resolution given), fills it with stripes, makes C:\STSHOW.2 and waits;
| puts the screen back, makes C:\STSHOW.3 and ends.
	.text
	.globl	_start
_start:	pea	clear
	bsr	cconws
	moveq	#23,d3
1:	pea	row
	bsr	cconws
	dbra	d3,1b
	pea	norm
	bsr	cconws
	pea	m1
	bsr	mark
	bsr	key
	move.w	#4,-(sp)		| Getrez
	trap	#14
	addq.l	#2,sp
	move.w	d0,d4
	move.w	#2,-(sp)		| Physbase
	trap	#14
	addq.l	#2,sp
	move.l	d0,d5
	move.w	#3,-(sp)		| Logbase
	trap	#14
	addq.l	#2,sp
	move.l	d0,d6
	move.l	#own+255,d7
	and.w	#0xff00,d7
	move.w	d4,-(sp)
	move.l	d7,-(sp)
	move.l	d7,-(sp)
	move.w	#5,-(sp)		| Setscreen(own, own, rez)
	trap	#14
	lea	12(sp),sp
	move.l	d7,a0
	move.w	#153600/4-1,d0
2:	move.l	#0x0f0f0f0f,(a0)+
	dbra	d0,2b
	pea	m2
	bsr	mark
	bsr	key
	move.w	d4,-(sp)
	move.l	d5,-(sp)
	move.l	d6,-(sp)
	move.w	#5,-(sp)		| Setscreen(log, phys, rez)
	trap	#14
	lea	12(sp),sp
	pea	m3
	bsr	mark
	clr.w	-(sp)			| Pterm0
	trap	#1
| cconws(s), s popped
cconws:	move.l	4(sp),-(sp)
	move.w	#9,-(sp)
	trap	#1
	addq.l	#6,sp
	move.l	(sp)+,(sp)
	rts
| an empty file named s, s popped
mark:	clr.w	-(sp)
	move.l	6(sp),-(sp)
	move.w	#0x3c,-(sp)		| Fcreate
	trap	#1
	addq.l	#8,sp
	tst.w	d0
	bmi.s	1f
	move.w	d0,-(sp)
	move.w	#0x3e,-(sp)		| Fclose
	trap	#1
	addq.l	#4,sp
1:	move.l	(sp)+,(sp)
	rts
key:	move.w	#2,-(sp)
	move.w	#2,-(sp)		| Bconin(2)
	trap	#13
	addq.l	#4,sp
	rts
	.data
clear:	.asciz	"\033E\033f\033w\033p"
row:	.asciz	"                                                                               \r\n"
norm:	.asciz	"\033H\033q"
m1:	.asciz	"C:\\STSHOW.1"
m2:	.asciz	"C:\\STSHOW.2"
m3:	.asciz	"C:\\STSHOW.3"
	.bss
	.even
own:	.space	153600+256
