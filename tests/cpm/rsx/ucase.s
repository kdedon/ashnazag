| ucase.s -- UCASE.RSX: function 2 output in upper case.  It counts the
| calls it sees; function 60 sub-function 200 answers the count.

	.text
	.globl	_start

_start:
	.short	0, 0, 0		| serial
	.long	entry
	.short	0x5253
next:	.long	0
	.long	0		| prev
	.byte	0, 0		| warm-boot flag, nbank
	.ascii	"UCASE   "
	.byte	0, 0		| endchain, patch
	.long	0		| up
	.short	0
	.long	0		| len

entry:
	addql	&1,ncalls
	cmpw	&2,%d0
	beq.b	fold
	cmpw	&60,%d0
	beq.b	fn60
down:
	movel	next,%sp@-
	rts
fold:
	cmpb	&0x61,%d1
	bcs.b	down
	cmpb	&0x7a,%d1
	bhi.b	down
	andb	&0xdf,%d1
	bra.b	down
fn60:
	movel	%a0,%sp@-
	movel	%d1,%a0
	cmpb	&200,%a0@
	movel	%sp@+,%a0
	bne.b	down
	movel	ncalls,%d0
	rte

	.data
ncalls:	.long	0
