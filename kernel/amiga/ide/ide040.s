| ide_cpush(pa, n): push and invalidate the data cache pages over [pa, pa+n)

	.text
	.globl	ide_cpush
ide_cpush:
	movel	%sp@(4),%d0
	movel	%sp@(8),%d1
	beq	Ldone
	addl	%d0,%d1
	andl	&0xfffff000,%d0
Lpage:
	movel	%d0,%a0
	.word	0xf470			| cpushp dc,(%a0)
	addl	&4096,%d0
	cmpl	%d1,%d0
	bcs	Lpage
Ldone:
	rts
	.balign	4
