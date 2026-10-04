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
	tstl	mac_slotstuck		| CA1 masked: serve slots, retry
	beq.w	Lp1nostuck
	jsr	mac_slotretry
Lp1nostuck:
	clrl	mac_storm+12		| stray levels: storm counts per tick
	clrl	mac_storm+20
	clrl	mac_storm+24
.ifdef BOOTDIAG
	jsr	diag_tick
.endif
	addql	&8,%sp
	jmp	intret
Lp1adb:
.ifdef BOOTDIAG
	addql	&1,diag_nadb
.endif
	movew	&0x2400,%sr		| state machine at IPL 4
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
.ifdef BOOTDIAG
	addql	&1,diag_nca1
.endif
	moveq	&7,%d2
Lp2loop:
	moveal	&0x50f02000,%a0
	moveb	%a0@(0x1e00),%d3	| port A, no handshake: slot lines, low = asserted
	moveb	%d3,%d0
	notb	%d0
	andib	&0x3e,%d0		| a card slot: no driver to clear it
	bne.w	Lp2card
	moveb	%d3,%d0
	andib	&0x41,%d0
	cmpib	&0x41,%d0
	beq.w	Lp2done
	btst	&0,%d3			| slot $9: on-board SONIC
	bne.w	Lp2vbl
	jsr	snintr			| clears the chip, so its line rises again
Lp2vbl:
	btst	&6,%d3			| built-in video
	bne.w	Lp2again
	jsr	ds_vblintr		| clears DAFB's interrupt
Lp2again:
	dbf	%d2,Lp2loop
	bra.w	Lp2mask
Lp2card:
	addql	&1,sn_nslot
	btst	&0,%d3
	bne.w	Lp2c1
	jsr	snintr
Lp2c1:
	btst	&6,%d3
	bne.w	Lp2mask
	jsr	ds_vblintr
|
| A line that stays low gives no further edge on a real VIA, so SONIC and
| VBL would go quiet; clones that present CA1 as a level would storm.
| Turn CA1 off and keep the lines for inspection.
|
Lp2mask:
	moveal	&0x50f02000,%a0
	movew	&0x100,mac_slotstuck+2	| nonzero even with every line low
	moveb	%a0@(0x1e00),mac_slotstuck+3
	moveb	&0x02,%a0@(0x1c00)
Lp2done:
	jmp	intret

|
| From the tick at IPL 2 while CA1 is masked: serve SONIC and VBL, and
| turn CA1 back on once the card lines are high.
|
	.globl	mac_slotretry
mac_slotretry:
	movel	%d3,%sp@-
	moveal	&0x50f02000,%a0
	moveb	%a0@(0x1e00),%d3
	btst	&0,%d3
	bne.w	Lsr1
	jsr	snintr
Lsr1:
	btst	&6,%d3
	bne.w	Lsr2
	jsr	ds_vblintr
Lsr2:
	moveal	&0x50f02000,%a0
	moveb	%a0@(0x1e00),%d3
	andib	&0x3e,%d3
	cmpib	&0x3e,%d3
	bne.w	Lsr3
	moveb	&0x02,%a0@(0x1a00)	| no stale edge
	moveb	&0x82,%a0@(0x1c00)
	clrl	mac_slotstuck
Lsr3:
	movel	%sp@+,%d3
	rts
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

|
| Levels 3, 5 and 6 have no source in the Mac OS mapping.  Count them and
| report the first; only a storm (65536 within one tick) panics.
|
	.globl	p3int
p3int:
	moveml	&0xfffe,%sp@-
	moveq	&3,%d2
	bra.w	Lpstray

	.globl	p5int
p5int:
	moveml	&0xfffe,%sp@-
	moveq	&5,%d2
	bra.w	Lpstray

	.globl	p6int
p6int:
	moveml	&0xfffe,%sp@-
	moveq	&6,%d2
Lpstray:
	movel	%d2,%d0
	lslw	&2,%d0
	lea	mac_spurious,%a0
	addql	&1,%a0@(0,%d0:w)
	cmpl	&1,%a0@(0,%d0:w)
	bne.w	Lpsmany
	movel	%d2,%sp@-
	pea	Lpsmsg
	jsr	printf
	addql	&8,%sp
	jmp	intret
Lpsmany:
	lea	mac_storm,%a0		| cleared by each tick
	addql	&1,%a0@(0,%d0:w)
	cmpl	&0x10000,%a0@(0,%d0:w)
	blt.w	Lpsout
	movel	%d2,%sp@-
	pea	Lpspanic
	jsr	panic
Lpsout:
	jmp	intret

	.balign	4
	.data
	.globl	mac_slotstuck
mac_slotstuck:	.long	0
mac_storm:	.long	0, 0, 0, 0, 0, 0, 0, 0
Lpsmsg:	.asciz	"mac: stray level-%d interrupt\n"
Lpspanic:	.asciz	"mac: level-%d interrupt storm"
	.balign	4
