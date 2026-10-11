| prot.s -- PROT.RSX: refuses function 19 and counts function 15's
| answers after the BDOS gives them.  Function 60 sub-function 201
| answers opens, failures and deletes as hex digits.

	.text
	.globl	_start

_start:
	.short	0, 0, 0		| serial
	.long	entry
	.short	0x5253
next:	.long	0
	.long	0		| prev
	.byte	0, 0		| warm-boot flag, nbank
	.ascii	"PROT    "
	.byte	0, 0		| endchain, patch
	.long	0		| up
	.short	0
	.long	0		| len

entry:
	cmpw	&19,%d0
	beq.b	refuse
	cmpw	&15,%d0
	beq.b	openit
	cmpw	&60,%d0
	beq.b	fn60
down:
	movel	next,%sp@-
	rts
refuse:
	addql	&1,ndel
	movel	&255,%d0
	rte
| a frame of our own, so the call comes back here
openit:
	movew	%sp@(6),%sp@-
	pea	back
	movew	%sp@(6),%sp@-
	bra.b	down
back:
	cmpb	&255,%d0
	beq.b	ofail
	addql	&1,nopen
	rte
ofail:
	addql	&1,nfail
	rte
fn60:
	movel	%a0,%sp@-
	movel	%d1,%a0
	cmpb	&201,%a0@
	movel	%sp@+,%a0
	bne.b	down
	movel	nopen,%d0
	lsll	&4,%d0
	addl	nfail,%d0
	lsll	&4,%d0
	addl	ndel,%d0
	rte

	.data
nopen:	.long	0
nfail:	.long	0
ndel:	.long	0
