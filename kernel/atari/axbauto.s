| axbauto.s -- \AUTO\AXBOOT.PRG for EmuTOS, which never runs a hard
| disk's boot code: reads the boot disk's root sector and calls it as
| TOS 3 and 4 do (a0 = sector, d3 = 'DMAr', d4 = DMAread device).
| Other TOSes ran the root sector already; it exits there.
	.text
	.globl	_start
_start:	.word	0x601a
	.long	_etext - code, 0, 1024 + 4096, 0, 0, 0
	.word	1			| no relocations
code:	moveal	%sp@(4),%a5		| basepage
	moveal	%a5@(0x18),%a6		| bss: 1 KB sector buffer, then stack
	lea	%a6@(1024 + 4096),%sp
	movel	%a5@(0x0c),%d0
	addl	%a5@(0x14),%d0
	addl	%a5@(0x1c),%d0
	addl	#0x100,%d0
	movel	%d0,%sp@-
	movel	%a5,%sp@-
	clrw	%sp@-
	movew	#0x4a,%sp@-		| Mshrink
	trap	#1
	lea	%sp@(12),%sp
	clrl	%sp@-
	movew	#0x20,%sp@-		| Super
	trap	#1
	addql	#6,%sp
	movel	%d0,%sp@-
	moveal	0x4f2.w,%a0
	cmpil	#0x45544f53,%a0@(0x2c)	| 'ETOS'
	bnes	out

| XHInqDev(boot drive): its DMAread device
	moveal	0x5a0.w,%a0
1:	movel	%a0@+,%d0
	beqs	out
	cmpil	#0x58484449,%d0		| 'XHDI'
	beqs	2f
	addql	#4,%a0
	bras	1b
2:	moveal	%a0@,%a1
	cmpil	#0x27011992,%a1@(-4)
	bnes	out
	movew	#0x19,%sp@-		| Dgetdrv
	trap	#1
	addql	#2,%sp
	clrw	%a6@			| major
	clrl	%sp@-			| bpb
	clrl	%sp@-			| start
	clrl	%sp@-			| minor
	movel	%a6,%sp@-		| major
	movew	%d0,%sp@-
	movew	#7,%sp@-		| XHInqDev
	jsr	%a1@
	lea	%sp@(20),%sp
	tstl	%d0
	bnes	out
	moveq	#0,%d4
	movew	%a6@,%d4

| DMAread(0, 1, buf, d4); run it if bootable
	movew	%d4,%sp@-
	movel	%a6,%sp@-
	movew	#1,%sp@-
	clrl	%sp@-
	movew	#42,%sp@-
	trap	#14
	lea	%sp@(14),%sp
	tstl	%d0
	bnes	out
	moveal	%a6,%a0
	moveq	#0,%d0
	movew	#255,%d1
3:	addw	%a0@+,%d0
	dbf	%d1,3b
	cmpw	#0x1234,%d0
	bnes	out
	moveal	%a6,%a0
	movel	#0x444d4172,%d3		| 'DMAr'
	jsr	%a6@
out:	movew	#0x20,%sp@-		| Super(old ssp)
	trap	#1
	addql	#6,%sp
	clrw	%sp@-			| Pterm0
	trap	#1
	.globl	_etext
_etext:
