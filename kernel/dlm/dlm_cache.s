| dlm_cache.s -- publish freshly relocated module text to the CPU, and
| the default platform hook called by dmainit.
|
| Whole-cache, at IPL 7:
|   68060  cpusha bc, then CACR.CABC (clear the branch cache)
|   68040  cpusha bc (pushes dirty data lines, invalidates both caches)
|   68020/030  CACR.CD | CACR.CI
| cputype (a long) comes from the 040/060 port; a base without it runs
| the 020/030 path.

	.text
	.weak	cputype

	.globl	dlm_cacheflush
dlm_cacheflush:
	movew	%sr,%d1
	movew	&0x2700,%sr
	clrl	%d0
	lea	cputype,%a0
	cmpal	&0,%a0
	beq.b	L1
	movel	%a0@,%d0
L1:	cmpil	&60,%d0
	blt.b	L2
	.word	0xf4f8			| cpusha bc
	.long	0x4e7a0002		| movec %cacr,%d0
	oril	&0x00400000,%d0		| CABC
	.long	0x4e7b0002		| movec %d0,%cacr
	bra.b	L4
L2:	cmpil	&40,%d0
	blt.b	L3
	.word	0xf4f8			| cpusha bc
	bra.b	L4
L3:	.long	0x4e7a0002		| movec %cacr,%d0
	oriw	&0x0808,%d0		| CD | CI
	.long	0x4e7b0002		| movec %d0,%cacr
L4:	movew	%d1,%sr
	rts

	.weak	plat_dmainit
	.globl	plat_dmainit
plat_dmainit:
	rts
