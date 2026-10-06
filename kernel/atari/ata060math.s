| ata060math.s -- 32-bit multiply and divide for objects built -m68000.
| The 68060 lacks the 64-bit forms the compiler uses for constant
| division; these use only the 32-bit ones.  Clobber d0-d1.

	.text
	.globl	__mulsi3
__mulsi3:
	movel	%sp@(4),%d0
	mulsl	%sp@(8),%d0
	rts

	.globl	__divsi3
__divsi3:
	movel	%sp@(4),%d0
	divsl	%sp@(8),%d0
	rts

	.globl	__udivsi3
__udivsi3:
	movel	%sp@(4),%d0
	divul	%sp@(8),%d0
	rts

	.globl	__modsi3
__modsi3:
	movel	%sp@(4),%d0
	divsll	%sp@(8),%d1,%d0
	movel	%d1,%d0
	rts

	.globl	__umodsi3
__umodsi3:
	movel	%sp@(4),%d0
	divull	%sp@(8),%d1,%d0
	movel	%d1,%d0
	rts
