| modadm(2): system call 68, carry set on error; retried on ERESTART.

	.text
	.globl	_modadm
	.weak	modadm
_modadm:
modadm:
	moveq	&68,%d0
	trap	&0
	bcc.b	Lmodadm
	cmpib	&91,%d0
	beq.b	_modadm
	jbra	_cerror
Lmodadm:
	rts
