| ea16.s -- 16-bit addressing: rm's registers added to d5.  The core
| that includes it sets RBX, RBP, RSI and RDI to their words' offsets
| from a5, and goes through Lea16 by rm, mod 0 rm 6 being its own.
Lea16:	.word	1f-Lea16, 2f-Lea16, 3f-Lea16, 4f-Lea16, 5f-Lea16, 6f-Lea16, 7f-Lea16, 8f-Lea16
1:	addw	%a5@(RBX),%d5		| BX + SI
	addw	%a5@(RSI),%d5
	rts
2:	addw	%a5@(RBX),%d5		| BX + DI
	addw	%a5@(RDI),%d5
	rts
3:	addw	%a5@(RBP),%d5		| BP + SI
	addw	%a5@(RSI),%d5
	rts
4:	addw	%a5@(RBP),%d5		| BP + DI
	addw	%a5@(RDI),%d5
	rts
5:	addw	%a5@(RSI),%d5		| SI
	rts
6:	addw	%a5@(RDI),%d5		| DI
	rts
7:	addw	%a5@(RBP),%d5		| BP
	rts
8:	addw	%a5@(RBX),%d5		| BX
	rts
