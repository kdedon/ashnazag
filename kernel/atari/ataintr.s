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

| Any other MFP channel (none is enabled): count it.  MFP handlers do
| not nest, so clearing every in-service bit ends just this one.
	.globl	ata_mfpstray
ata_mfpstray:
	clrb	MFP+ISRA
	clrb	MFP+ISRB
	addql	&1,ata_spurious+28
	rte

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

	.data
Lbp_vec:	.long	0
Lbp_sp:		.long	0
	.globl	ata_spurious
	.globl	ata_ticks
	.globl	ata_vbls
ata_spurious:	.long	0, 0, 0, 0, 0, 0, 0, 0
ata_ticks:	.long	0
ata_tdiv:	.long	4
ata_vbls:	.long	0
	.balign	4
