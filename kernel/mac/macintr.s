| macintr.s -- Quadra autovector handlers (replace the Amiga p1int..p6int).
|
| Q800 levels: 1 VIA1 (tick = Timer 1, ADB = shift register),
| 2 VIA2 (CB2 = 53C96 SCSI; CA1 = NuBus-slot lines, slot $9 = SONIC,
|   bit 6 = built-in video VBL),
| 4 SCC (sccintr); 3, 5, 6 unused.
| Entry and exit follow the stock handlers: save d0-d7/a0-a6, load
| sup_cacr, leave through intret.  Handlers get a pcb pointer built from a
| fake USP slot.

	.text

	.globl	p1int
p1int:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	moveal	&0x50f00000,%a0
	moveb	%a0@(0x1a00),%d0	| IFR
	andb	%a0@(0x1c00),%d0	| & IER
	btst	&2,%d0			| SR: ADB byte done, served first
	bne.w	Lp1adb
	btst	&6,%d0
	beq.w	Lp1other
	moveb	&0x40,%a0@(0x1a00)	| ack T1
	movew	&0x2200,%sr		| clock at IPL 2, as on the Amiga: no SCSI nesting
	addql	&1,mac_ticks
	jsr	adb_tick		| ADB watchdog, key repeat
	clrl	%sp@-
	pea	%sp@
	jsr	clock_int
	tstl	%d0
	beq.w	Lp1done
	jsr	addupc_clk
Lp1done:
	addql	&8,%sp
	jmp	intret
Lp1adb:
	movew	&0x2400,%sr		| state machine at IPL 4, as A/UX does
	jsr	adb_intr
	movew	&0x2100,%sr
	jsr	adb_soft		| completions and key/mouse data, IPL 1
	jmp	intret			| a pending tick re-enters p1int
Lp1other:
	andib	&0x3f,%d0
	beq.w	Lp1none
	moveb	%d0,%a0@(0x1c00)	| disable what nobody handles
	moveb	%d0,%a0@(0x1a00)
Lp1none:
	addql	&1,mac_spurious+4
	jmp	intret

	.globl	p2int
p2int:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	moveal	&0x50f02000,%a0
	moveb	%a0@(0x1a00),%d0
	andb	%a0@(0x1c00),%d0
	btst	&3,%d0			| CB2: 53C96 SCSI
	beq.w	Lp2slot
	moveb	&0x08,%a0@(0x1a00)	| ack the edge, then service the chip
	jsr	ncr96intr
	jmp	intret
Lp2slot:
	btst	&1,%d0			| CA1: a slot line fell
	beq.w	Lp2other
	moveb	&0x02,%a0@(0x1a00)	| ack the edge before reading the lines
	moveb	%a0@(0x1e00),%d3	| port A, no handshake: slot lines, low = asserted
	moveb	%d3,%d0
	andib	&0x41,%d0
	cmpib	&0x41,%d0
	bne.w	Lp2serve
	addql	&1,sn_nslot		| another slot (a card): not handled
	jmp	intret
| CA1 falls only when all lines were high: serve every line found low,
| then read again until SONIC and VBL are both released (8 rounds).
Lp2serve:
	moveq	&7,%d2
Lp2loop:
	btst	&0,%d3			| slot $9: on-board SONIC
	bne.w	Lp2vbl
	jsr	snintr			| clears the chip, so its line rises again
Lp2vbl:
	btst	&6,%d3			| built-in video
	bne.w	Lp2again
	jsr	ds_vblintr		| clears DAFB's interrupt
Lp2again:
	moveal	&0x50f02000,%a0
	moveb	%a0@(0x1e00),%d3
	moveb	%d3,%d0
	andib	&0x41,%d0
	cmpib	&0x41,%d0
	dbeq	%d2,Lp2loop
	jmp	intret
Lp2other:
	andib	&0x7f,%d0
	moveb	%d0,%a0@(0x1c00)
	moveb	%d0,%a0@(0x1a00)
	addql	&1,mac_spurious+8
	jmp	intret

	.globl	p4int
p4int:
	moveml	&0xfffe,%sp@-
	movel	sup_cacr,%d0
	movec	%d0,%cacr
	jsr	sccintr
	jmp	intret

	.globl	p3int
p3int:
	moveml	&0xfffe,%sp@-
	pea	Lp3msg
	jsr	panic

	.globl	p5int
p5int:
	moveml	&0xfffe,%sp@-
	pea	Lp5msg
	jsr	panic

	.globl	p6int
p6int:
	moveml	&0xfffe,%sp@-
	pea	Lp6msg
	jsr	panic

	.balign	4
	.data
Lp3msg:	.asciz	"mac: unexpected level-3 interrupt"
Lp5msg:	.asciz	"mac: unexpected level-5 interrupt"
Lp6msg:	.asciz	"mac: unexpected level-6 interrupt"
	.balign	4
