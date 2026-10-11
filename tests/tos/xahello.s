| xahello.s -- XAHELLO.TOS: a plain TOS program for the console window.
| Asks for a line (Cconrs), writes it to C:\XAHELLO.TXT, ends with 5.
	.text
	.globl	_start
_start:	pea	hello
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
	move.w	#5,-(sp)
	move.w	#0x4c,-(sp)		| Pterm
	trap	#1
	.data
hello:	.asciz	"Hello from TOS\r\nName? "
bye:	.asciz	"\r\nBye\r\n"
fname:	.asciz	"C:\\XAHELLO.TXT"
	.even
line:	.byte	40, 0
	.space	42
