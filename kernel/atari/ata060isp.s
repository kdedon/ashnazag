| ata060isp.s -- vector 61 on the 060.  Kernel and module code divides
| by a constant with the 64-bit muls.l/mulu.l, which the 060 lacks; it
| gets them emulated here, operand Dn, (An), (d16,An) or #imm, read with
| supervisor accesses, and ata_isp61_n counts them.  User traps and
| anything else go on to the handler it replaced, ata_isp61_next.

	.text
	.globl	ata_isp61
ata_isp61:
	btst	&5,%sp@			| from supervisor mode?
	beq.w	Li61_user
	moveml	&0xfffe,%sp@-		| d0-d7/a0-a6; SR at 60, PC at 62
	movel	%sp@(62),%a0
	movew	%a0@,%d0
	movew	%d0,%d1
	andiw	&0xffc0,%d1
	cmpiw	&0x4c00,%d1		| mul.l
	bne.w	Li61_no
	movew	%a0@(2),%d1		| 0 Dl:3 signed 64-bit 0:7 Dh:3
	btst	&10,%d1
	beq.w	Li61_no
	movew	%d1,%d2
	andiw	&0x83f8,%d2
	bne.w	Li61_no
	movew	%d0,%d2
	andiw	&7,%d2			| EA register
	lsrw	&3,%d0
	andiw	&7,%d0			| EA mode
	moveq	&4,%d7			| instruction length
	tstw	%d0
	bne.s	1f
	movel	%sp@(0,%d2:w:4),%d4	| Dn
	bra.s	5f
1:	cmpiw	&7,%d0
	bne.s	2f
	cmpiw	&4,%d2
	bne.w	Li61_no
	movel	%a0@(4),%d4		| #imm
	moveq	&8,%d7
	bra.s	5f
2:	cmpiw	&7,%d2
	beq.s	3f
	moveal	%sp@(32,%d2:w:4),%a1
	bra.s	4f
3:	lea	%sp@(68),%a1		| sp before the 8-byte frame
4:	cmpiw	&2,%d0
	beq.s	6f
	cmpiw	&5,%d0
	bne.w	Li61_no
	addaw	%a0@(4),%a1
	moveq	&6,%d7
6:	movel	%a1@,%d4
5:	movew	%d1,%d5
	rolw	&4,%d5
	andiw	&7,%d5			| Dl
	movel	%d4,%a2			| a
	movel	%sp@(0,%d5:w:4),%d3	| b
	movel	%d3,%a3
	| 32x32 -> 64 from 16-bit products: hi d0, lo d6
	movel	%d4,%d2
	movel	%d2,%d6
	mulu.w	%d3,%d6			| al*bl
	movel	%d2,%d0
	swap	%d0
	movel	%d3,%d4
	swap	%d4
	mulu.w	%d4,%d0			| ah*bh
	mulu.w	%d2,%d4			| al*bh
	swap	%d2
	mulu.w	%d3,%d2			| ah*bl
	addl	%d4,%d2
	bcc.s	7f
	addil	&0x10000,%d0
7:	movel	%d2,%d4
	swap	%d4
	clrw	%d4
	clrw	%d2
	swap	%d2
	addl	%d4,%d6
	addxl	%d2,%d0
	btst	&11,%d1			| signed: hi -= (a < 0 ? b : 0) + (b < 0 ? a : 0)
	beq.s	8f
	movel	%a2,%d2
	bpl.s	9f
	subl	%a3,%d0
9:	movel	%a3,%d2
	bpl.s	8f
	subl	%a2,%d0
8:	movel	%d6,%sp@(0,%d5:w:4)
	andiw	&7,%d1
	movel	%d0,%sp@(0,%d1:w:4)	| Dh
	addql	&1,ata_isp61_n
	movew	%sp@(60),%d2		| N and Z of the product, V and C clear
	andiw	&0xfff0,%d2
	tstl	%d0
	bpl.s	1f
	oriw	&8,%d2
1:	orl	%d6,%d0
	bne.s	2f
	oriw	&4,%d2
2:	movew	%d2,%sp@(60)
	addl	%d7,%sp@(62)
	moveml	%sp@+,&0x7fff
	rte
Li61_no:
	moveml	%sp@+,&0x7fff
Li61_user:
	movel	ata_isp61_next,%sp@-
	rts

	.data
	.globl	ata_isp61_n
	.globl	ata_isp61_next
ata_isp61_n:	.long	0
ata_isp61_next:	.long	isp61_vec
