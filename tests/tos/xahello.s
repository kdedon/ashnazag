| xahello.s -- XAHELLO.TOS: a plain TOS program for the console window.
| Asks for a line (Cconrs), writes it to C:\XAHELLO.TXT, ends with 5,
| or 6 when its TPA is 256 KB or less (a MiNT-aware program's).
	.text
	.globl	_start
_start:	move.l	4(sp),a0
	move.l	4(a0),d4		| p_hitpa - p_lowtpa
	sub.l	(a0),d4
	pea	hello
	move.w	#9,-(sp)		| Cconws
	trap	#1
	addq.l	#6,sp
	pea	line
	move.w	#10,-(sp)		| Cconrs
	trap	#1
	addq.l	#6,sp
	clr.w	-(sp)
	pea	fname
	move.w	#0x3c,-(sp)		| Fcreate
	trap	#1
	addq.l	#8,sp
	move.w	d0,d3
	bmi.s	1f
	pea	line+2
	moveq	#0,d0
	move.b	line+1,d0
	move.l	d0,-(sp)
	move.w	d3,-(sp)
	move.w	#0x40,-(sp)		| Fwrite
	trap	#1
	lea	12(sp),sp
	move.w	d3,-(sp)
	move.w	#0x3e,-(sp)		| Fclose
	trap	#1
	addq.l	#4,sp
1:	pea	bye
	move.w	#9,-(sp)
	trap	#1
	addq.l	#6,sp
	moveq	#5,d0
	cmp.l	#0x40000,d4
	bhi.s	2f
	moveq	#6,d0
2:	move.w	d0,-(sp)
	move.w	#0x4c,-(sp)		| Pterm
	trap	#1
	.data
hello:	.asciz	"Hello from TOS\r\nName? "
bye:	.asciz	"\r\nBye\r\n"
fname:	.asciz	"C:\\XAHELLO.TXT"
	.even
line:	.byte	40, 0
	.space	42
