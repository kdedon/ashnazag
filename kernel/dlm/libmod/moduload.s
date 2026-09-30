| moduload(2): system call 65, carry set on error; retried on ERESTART.

	.text
	.globl	_moduload
	.weak	moduload
_moduload:
moduload:
	moveq	&65,%d0
	trap	&0
	bcc.b	Lmoduload
	cmpib	&91,%d0
	beq.b	_moduload
	jbra	_cerror
Lmoduload:
	rts
