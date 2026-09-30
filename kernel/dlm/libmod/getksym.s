| getksym(2): system call 69, carry set on error; retried on ERESTART.

	.text
	.globl	_getksym
	.weak	getksym
_getksym:
getksym:
	moveq	&69,%d0
	trap	&0
	bcc.b	Lgetksym
	cmpib	&91,%d0
	beq.b	_getksym
	jbra	_cerror
Lgetksym:
	rts
