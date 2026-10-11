| rsxcrt.s -- the header and trap entry of an RSX written in C.
|
| rsx(fn, param) answers a call with its result, or sets rsxdown to
| pass the call on.  rsxbdos(fn, param) calls the handler above this
| module.  After the header: an echo flag and a file's FCB, which the
| program that attaches the module fills in.

	.text
	.globl	_start, rsxwarm, rsxecho, rsxfcb, rsxdown, rsxbdos, rsx

_start:
	.short	0, 0, 0		| serial
	.long	entry
	.short	0x5253
next:	.long	0
	.long	0		| prev
rsxwarm:
	.byte	0, 0		| warm-boot flag, nbank
	.ascii	"NAME    "
	.byte	0, 0		| endchain, patch
	.long	0		| up
	.short	0
	.long	0		| len
rsxecho:
	.byte	1, 0
rsxfcb:
	.long	0, 0, 0, 0, 0, 0, 0, 0, 0

entry:
	moveml	&0xfffe,%sp@-		| d0-d7/a0-a6
	movew	%sp@(60),savesr
	movel	%d1,%sp@-
	movew	%d0,%sp@-
	jsr	rsx
	addql	&6,%sp
	tstw	rsxdown
	bne.b	Lpass
	movel	%d0,%sp@
	moveml	%sp@+,&0x7fff
	rte
Lpass:
	clrw	rsxdown
	moveml	%sp@+,&0x7fff
	movel	next,%sp@-
	rts

| a trap frame of our own, so the call comes back here
rsxbdos:
	movew	%sp@(4),%d0
	movel	%sp@(6),%d1
	movew	&0x88,%sp@-
	pea	Lback
	movew	savesr,%sp@-
	movel	next,%sp@-
	rts
Lback:
	rts

	.data
savesr:	.short	0
rsxdown: .short	0
