| cpment.s -- startcpm's BIOS entry (trap #3) and CP/M's start.
|
| CP/M runs in virtual supervisor mode; a trap's format-0 frame is on
| its stack.  The BIOS's C runs on a stack of its own and returns by
| rte, to user or supervisor state as the frame says.  Warm boot does
| not return: the CCP starts over on its own stack.

	.text
	.globl	cpm_t3, cpm_go

cpm_t3:
	cmpw	&1,%d0
	bne.b	Lbios
	clrl	%d0
	movel	cpm_ccp,%a0
	jmp	%a0@
Lbios:
	moveml	&0x7ffe,%sp@-		| d1-d7/a0-a6
	movel	%sp,%a0
	movel	&cpm_stk+0x8000,%sp
	movel	%a0,%sp@-
	movel	%d2,%sp@-
	movel	%d1,%sp@-
	movel	%d0,%sp@-
	jsr	cpm_bios
	addl	&12,%sp
	movel	%sp@,%sp
	moveml	%sp@+,&0x7ffe
	rte

| cpm_go(pc)
cpm_go:
	movel	%sp@(4),%a0
	jmp	%a0@
