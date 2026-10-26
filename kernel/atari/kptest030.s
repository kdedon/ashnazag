| MMUSR for a kernel fault, walked from the supervisor root.  I is always
| set: a fault on a kernel page the tables map is stale and reloads too.
	.text
	.globl	kptest
kptest:
	moveal	%sp@(4),%a0
	.word	0xf010,0x9e15		| ptestr #5,(%a0),#7
	.word	0xf02f,0x6200,0x0004	| pmove %psr,%sp@(4)
	moveq	&0,%d0
	movew	%sp@(4),%d0
	oriw	&0x400,%d0
	moveal	%d0,%a0
	rts
