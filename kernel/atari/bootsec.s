| bootsec.s -- AHDI root-sector boot code, at most 342 bytes so ICD
| entries from 0x156 stay free.
|
| TOS 3 and 4 run it from the disk buffer with a0 = this sector,
| d3 = 'DMAr' (XBIOS DMAread works) and d4 = the DMAread device.  Loads
| the loader (16 sectors) from the start of the AXB partition and calls
| loader(device, AXB start, this sector).  If it returns, TOS was chosen:
| boot the first TOS-bootable partition (flags 0x81) the standard way,
| from the second half of the disk buffer, then return to the ROM.
	.text
	.globl	_start
_start:
	moveml	%d0-%d7/%a0-%a6,%sp@-
	lea	%pc@(_start),%a6
	cmpil	#0x444d4172,%d3
	jbne	out
	lea	%a6@(0x1c6),%a3
	moveq	#3,%d6
1:	movel	%a3@,%d0
	andil	#0x01ffffff,%d0
	cmpil	#0x01415842,%d0		| exists, "AXB"
	beqs	2f
	lea	%a3@(12),%a3
	dbf	%d6,1b
	bras	tos
2:	movel	#8192,%sp@-
	movew	#0x48,%sp@-		| Malloc
	trap	#1
	addql	#6,%sp
	tstl	%d0
	bles	tos
	moveal	%d0,%a5
	moveal	%d0,%a4
	moveq	#16,%d5
	bsrs	rd
	bnes	3f
	cmpil	#0x41584232,%a5@	| 'AXB2'
	bnes	3f
	movel	%a6,%sp@-
	movel	%a3@(4),%sp@-
	movel	%d4,%sp@-
	jsr	%a5@(4)
	lea	%sp@(12),%sp
3:	movel	%a5,%sp@-
	movew	#0x49,%sp@-		| Mfree
	trap	#1
	addql	#6,%sp

tos:	lea	%a6@(0x1c6),%a3
	moveq	#3,%d6
1:	moveb	%a3@,%d0
	andib	#0x81,%d0
	cmpib	#0x81,%d0
	beqs	2f
	lea	%a3@(12),%a3
	dbf	%d6,1b
	bras	out
2:	lea	%a6@(512),%a4
	moveq	#1,%d5
	bsrs	rd
	bnes	out
	moveq	#0,%d0
	move	#255,%d1
3:	addw	%a4@+,%d0
	dbf	%d1,3b
	cmpw	#0x1234,%d0
	bnes	out
	moveml	%sp@,%d0-%d7/%a0-%a6
	lea	%a0@(512),%a0
	jsr	%a0@
out:	moveml	%sp@+,%d0-%d7/%a0-%a6
	rts

| DMAread d5 sectors from the start of partition a3 to a4; Z set if OK
rd:	movew	%d4,%sp@-
	movel	%a4,%sp@-
	movew	%d5,%sp@-
	movel	%a3@(4),%sp@-
	movew	#42,%sp@-
	trap	#14
	lea	%sp@(14),%sp
	tstl	%d0
	rts
