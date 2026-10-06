| exit.s -- C:\AUTO\EXITTEST.PRG: the session accessory's host calls.
| Asks the host to halt, then exits with 10 + the request's errno.
	.text
	.globl	_start
_start:	clr.l	-(sp)			| ioctl(/dev/tos, TOSIOC_HALT, 0)
	move.l	#0x5409,-(sp)
	move.l	0xfa0078,-(sp)
1:	moveq	#54,d0
	move.l	0xfa007c,a0		| the machine layer's host call
	jsr	(a0)
	bcc.s	2f
	cmp.l	#4,d0			| EINTR: the carrier signal interrupted it
	beq.s	1b
	bra.s	3f
2:	moveq	#0,d0
3:	lea	12(sp),sp
	add.l	#10,d0
	move.l	d0,-(sp)		| exit
4:	moveq	#1,d0
	move.l	0xfa007c,a0
	jsr	(a0)
	bra.s	4b
