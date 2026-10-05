| mintent.s -- trap entries of mintrun's MiNT calls, and the program start.
|
| The program runs in virtual supervisor mode, so a trap's format-0 frame
| (SR, PC, format/vector) sits on its own stack just below the arguments.
| The return skips rte: the frame is reshaped for rtr.

	.text
	.globl	mint_t1, mint_t2, mint_t13, mint_t14, mint_go

mint_t1:
	moveml	&0x7ffe,%sp@-		| d1-d7/a0-a6
	lea	mint_gemdos,%a1
	bra.b	Lcall
mint_t13:
	moveml	&0x7ffe,%sp@-
	lea	mint_bios,%a1
	bra.b	Lcall
mint_t14:
	moveml	&0x7ffe,%sp@-
	lea	mint_xbios,%a1
| The C side runs on mintrun's own stack, not the program's small one;
| a call from a signal handler already on it stays there.
Lcall:
	movel	%sp,%a0
	cmpal	&mint_stk,%sp
	bcs.b	Lsw
	cmpal	&mint_stk+0x10000,%sp
	bcs.b	Lon
Lsw:
	movel	&mint_stk+0x10000,%sp
Lon:
	movel	%a0,%sp@-
	pea	%a0@(64)		| the arguments, past the frame
	jsr	%a1@
	addql	&4,%sp
	movel	%sp@,%sp
	moveml	%sp@+,&0x7ffe
mint_t2:
	movel	%sp@(2),%sp@(4)		| PC over the format word
	movew	%sp@,%sp@(2)		| SR below it
	addql	&2,%sp
	rtr

| mint_go(pc, sp, basepage): TOS's start: basepage at 4(sp), a0 = 0
mint_go:
	movel	%sp@(4),%a1
	movel	%sp@(12),%d0
	movel	%sp@(8),%sp
	movel	%d0,%sp@-
	clrl	%sp@-
	subal	%a0,%a0
	jmp	%a1@
