| modpath(2): system call 66, carry set on error; retried on ERESTART.

	.text
	.globl	_modpath
	.weak	modpath
_modpath:
modpath:
	moveq	&66,%d0
	trap	&0
	bcc.b	Lmodpath
	cmpib	&91,%d0
	beq.b	_modpath
	jbra	_cerror
Lmodpath:
	rts
