| snsup.s -- 68040 data-cache maintenance for the SONIC's DMA areas.
|
| void sn_dcpush(char *p, long n)   write back and drop the lines of [p, p+n)
| void sn_dcinval(char *p, long n)  drop the lines of [p, p+n), no write-back
|
| The addresses are physical; the driver's pool is kernel .bss reached
| through DTT0, so VA = PA.  Each cpushl is followed by a cinvl so the
| line is gone whatever CPUSH does on the part (on a 68060 its
| invalidation depends on CACR.DPI).  DTT0 maps the pool cache-inhibited,
| so both are no-ops costing a few cycles a line.  Enough for buffers in
| any cache mode, for descriptors only in write-through.
| Opcodes as words: cpushl dc,(a0) = 0xf468, cinvl dc,(a0) = 0xf448.

	.text
	.globl	sn_dcpush
sn_dcpush:
	movel	%sp@(4),%d0		| p
	movel	%sp@(8),%d1		| n
	ble.w	Lpdone
	addl	%d0,%d1			| end
	andib	&0xf0,%d0
	moveal	%d0,%a0
Lploop:
	.word	0xf468			| cpushl dc,(a0)
	.word	0xf448			| cinvl dc,(a0)
	addaw	&16,%a0
	cmpal	%d1,%a0
	bcs.w	Lploop
Lpdone:
	rts

	.globl	sn_dcinval
sn_dcinval:
	movel	%sp@(4),%d0
	movel	%sp@(8),%d1
	ble.w	Lidone
	addl	%d0,%d1
	andib	&0xf0,%d0
	moveal	%d0,%a0
Liloop:
	.word	0xf448			| cinvl dc,(a0)
	addaw	&16,%a0
	cmpal	%d1,%a0
	bcs.w	Liloop
Lidone:
	rts
