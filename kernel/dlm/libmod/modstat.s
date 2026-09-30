| modstat(2): system call 67, carry set on error; retried on ERESTART.

	.text
	.globl	_modstat
	.weak	modstat
_modstat:
modstat:
	moveq	&67,%d0
	trap	&0
	bcc.b	Lmodstat
	cmpib	&91,%d0
	beq.b	_modstat
	jbra	_cerror
Lmodstat:
	rts
