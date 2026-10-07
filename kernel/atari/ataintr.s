| ataintr.s -- Falcon interrupt handlers (replace the Amiga p1int..p6int).
|
| Autovectors: 2 HBL, 4 VBL, 5 SCC (reset by config), 6 never (the MFP
| is vectored).  MFP vectors 64-79 are set by config: timer A (tick) and
| GPIP4 (IKBD ACIA); the rest count as strays.
| Entry and exit follow the stock handlers: save d0-d7/a0-a6, load
| sup_cacr, leave through intret.  Handlers get a pcb pointer built from a
| fake USP slot.

MFP	=	0xfffffa01
ISRA	=	0x0e
ISRB	=	0x10

	.text

| HBL requests on every scan line: return with the level raised to 2,
| so it stays masked until the next spl0.
	.globl	p2int
p2int:
	oriw	&0x0200,%sp@
	rte

	.globl	p4int
p4int:
	addql	&1,ata_vbls
	rte

	.globl	p1int
p1int:
	addql	&1,ata_spurious+4
	rte

	.globl	p3int
p3int:
	addql	&1,ata_spurious+12
	rte

	.globl	p5int
p5int:
	addql	&1,ata_spurious+20
	rte

	.globl	p6int
p6int:
	addql	&1,ata_spurious+24
	rte

| MFP timer A at 240 Hz; every fourth interrupt is the 60 Hz clock tick.
	.globl	ata_clkint
ata_clkint:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	moveb	&0xdf,MFP+ISRA		| end of interrupt: timer A
	addql	&1,ata_ticks
	subql	&1,ata_tdiv
	bgt.w	Lckdone
	movel	&4,ata_tdiv
	clrl	%sp@-
	pea	%sp@
	jsr	clock_int
	tstl	%d0
	beq.w	Lckpop
	jsr	addupc_clk
Lckpop:
	addql	&8,%sp
Lckdone:
	jmp	intret

| MFP GPIP4: keyboard/MIDI ACIAs.
	.globl	ata_aciaint
ata_aciaint:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	jsr	ikbd_intr
	moveb	&0xbf,MFP+ISRB		| end of interrupt: GPIP4
	jmp	intret

| MFP GPIP7: the DMA sound end, enabled only while a guest plays in front.
	.globl	ata_sndint
ata_sndint:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	jsr	ds_sndintr
	moveb	&0x7f,MFP+ISRA		| end of interrupt: GPIP7
	jmp	intret

| Any other MFP channel (none is enabled): count it.  MFP handlers do
| not nest, so clearing every in-service bit ends just this one.
	.globl	ata_mfpstray
ata_mfpstray:
	clrb	MFP+ISRA
	clrb	MFP+ISRB
	addql	&1,ata_spurious+28
	rte

| TT MFP, vectors 80-95: the channel's handler from ata_ttmfp, after
| its end of interrupt; a channel without one is counted and turned off.
TTMFP	=	0xfffffa81
IERA	=	0x06
IERB	=	0x08

	.globl	ata_ttmfpint
ata_ttmfpint:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	movew	%sp@(66),%d0		| format/vector word
	andiw	&0x03c,%d0		| channel * 4
	moveq	&-1,%d1
	lsrw	&2,%d0
	bclr	%d0,%d1			| ~(1 << channel)
	lslw	&3,%d0
	lea	ata_ttfn,%a0
	addaw	%d0,%a0
	cmpiw	&64,%d0
	bge.s	Ltt_a
	moveb	%d1,TTMFP+ISRB
	tstl	%a0@
	bne.s	Ltt_call
	andb	%d1,TTMFP+IERB
	bra.s	Ltt_stray
Ltt_a:
	rorw	&8,%d1
	moveb	%d1,TTMFP+ISRA
	tstl	%a0@
	bne.s	Ltt_call
	andb	%d1,TTMFP+IERA
Ltt_stray:
	addql	&1,ata_spurious+28
	jmp	intret
Ltt_call:
	movel	%a0@(4),%sp@-
	moveal	%a0@,%a0
	jsr	%a0@
	addql	&4,%sp
	jmp	intret

| int ata_spltty(): at least IPL 6 (masks the MFP), never lowers; returns
| the old SR for ata_splx.  No immediate move to SR: the spl site count
| covers those.
	.globl	ata_spltty
ata_spltty:
	movew	%sr,%d0
	movew	%d0,%d1
	andiw	&0x0700,%d1
	cmpiw	&0x0600,%d1
	bge.s	Lsplok
	movew	%d0,%d1
	andiw	&0xf8ff,%d1
	oriw	&0x0600,%d1
	movew	%d1,%sr
Lsplok:
	rts

	.globl	ata_splx
ata_splx:
	movew	%sp@(6),%sr
	rts

| ata_busprobe(addr): 1 if a byte read at addr completes, 0 on a bus
| error.  The bus error handler drops its frame and returns 0.
	.globl	ata_busprobe
ata_busprobe:
	movel	%sp@(4),%a0
	movew	%sr,%d1
	oriw	&0x0700,%sr
	movel	M68Kvec+8,Lbp_vec
	movel	%sp,Lbp_sp
	movel	&Lbp_fail,M68Kvec+8
	moveq	&1,%d0
	nop
	tstb	%a0@
	nop
Lbp_out:
	movel	Lbp_vec,M68Kvec+8
	movew	%d1,%sr
	rts
Lbp_fail:
	movel	Lbp_sp,%sp
	moveq	&0,%d0
	bras	Lbp_out

.ifdef ATA060
| ata_dmasync(): DMA (blitter, sound, disk DMA chip) reaches only
| ST-RAM, which is never data-cached, so there is nothing to push.
	.globl	ata_dmasync
ata_dmasync:
	rts

| ata_idcm(pa, cm): set the cache mode of FastRAM page pa in the identity
| map, then push and invalidate the page's lines.  Pages outside the map
| are left alone.  Clobbers d0/d1/a0/a1.
	.globl	ata_idcm
ata_idcm:
	movel	%sp@(4),%d1
	andil	&0xfffff000,%d1
	moveal	%d1,%a0
	tstl	ata_idpt
	beq.s	Lidcm_ret
	subl	ata_idlo,%d1
	bcs.s	Lidcm_ret
	cmpal	ata_idhi,%a0
	bcc.s	Lidcm_ret
	moveq	&10,%d0
	lsrl	%d0,%d1
	moveal	ata_idpt,%a1
	addal	%d1,%a1
	movel	%a1@,%d1
	btst	&0,%d1
	beq.s	Lidcm_ret
	movel	%d1,%d0
	andil	&0x60,%d0
	cmpl	%sp@(8),%d0
	beq.s	Lidcm_ret
	eorl	%d0,%d1
	orl	%sp@(8),%d1
	movew	%sr,%d0
	movew	&0x2700,%sr
	movel	%d1,%a1@
	nop
	.word	0xf518			| pflusha
	.word	0xf470			| cpushp dc,(%a0)
	movew	%d0,%sr
Lidcm_ret:
	rts

| Table pages and u-areas are noncacheable in the identity map, as the
| table walk and the u-area windows bypass the data cache; page_free
| gives a page back its copyback mode.
	.globl	hat_ptalloc
hat_ptalloc:
	movel	%sp@(8),%sp@-
	movel	%sp@(8),%sp@-
	jsr	ata_ptalloc_mac
	addql	&8,%sp
	tstl	%d0
	beq.s	Lpta_ret
	movel	%d0,%sp@-
	pea	0x40
	movel	%d0,%sp@-
	jsr	ata_idcm
	addql	&8,%sp
	movel	%sp@+,%d0
Lpta_ret:
	moveal	%d0,%a0
	rts

	.globl	segu_get
segu_get:
	movel	%sp@(4),%sp@-
	jsr	__amix_segu_get
	addql	&4,%sp
	tstl	%d0
	beq.s	Lsg_ret
	movel	%d0,%sp@-
	movel	%sp@(8),%a0
	bsr.s	Lunc_u
	movel	%sp@+,%d0
Lsg_ret:
	moveal	%d0,%a0
	rts

	.globl	swapinub
swapinub:
	movel	%sp@(4),%sp@-
	jsr	__amix_swapinub
	addql	&4,%sp
	movel	%d0,%sp@-
	movel	%sp@(8),%a0
	bsr.s	Lunc_u
	movel	%sp@+,%d0
	moveal	%d0,%a0
	rts

| a0 = proc: its two u-area pages, from p_ubptbl entries 0 and 2
Lunc_u:
	movel	%a2,%sp@-
	movel	%a0,%d0
	beq.s	Lunc_ret
	addil	&95,%d0
	andil	&0xfffffff0,%d0
	moveal	%d0,%a2
	movel	%a2@,%d0
	bsr.s	Lunc_pg
	movel	%a2@(8),%d0
	bsr.s	Lunc_pg
Lunc_ret:
	moveal	%sp@+,%a2
	rts
Lunc_pg:
	btst	&0,%d0
	beq.s	Lunc_pgret
	pea	0x40
	movel	%d0,%sp@-
	jsr	ata_idcm
	addql	&8,%sp
Lunc_pgret:
	rts

	.globl	page_free
page_free:
	movel	%sp@(4),%d0
	subl	pages,%d0
	bcs.s	Lpf_go
	movel	%sp@(4),%d1
	cmpl	epages,%d1
	bcc.s	Lpf_go
	divul	&60,%d0			| sizeof (page_t)
	addl	pages_base,%d0
	moveq	&12,%d1
	lsll	%d1,%d0
	movel	hat_cm_ram,%sp@-
	movel	%d0,%sp@-
	jsr	ata_idcm
	addql	&8,%sp
Lpf_go:
	jmp	__amix_page_free

| Vector 61 on the 060.  Kernel code from the stock image divides by a
| constant with the 64-bit muls.l/mulu.l, which the 060 lacks; it gets
| them emulated here, operand Dn, (An), (d16,An) or #imm, and ata_isp61_n
| counts them.  User traps and anything else go on to isp61_vec.
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
	jmp	isp61_vec
.endif

	.data
.ifdef ATA060
	.globl	ata_isp61_n
ata_isp61_n:	.long	0
.endif
Lbp_vec:	.long	0
Lbp_sp:		.long	0
	.globl	ata_spurious
	.globl	ata_ticks
	.globl	ata_vbls
ata_spurious:	.long	0, 0, 0, 0, 0, 0, 0, 0
| TT MFP channel handlers: function, argument
	.globl	ata_ttfn
ata_ttfn:	.space	128
ata_ticks:	.long	0
ata_tdiv:	.long	4
ata_vbls:	.long	0
	.balign	4
