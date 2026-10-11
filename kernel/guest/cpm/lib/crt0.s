| crt0.s -- start of a CP/M-68K C program and its BDOS and BIOS gates.
|
| The loader enters at the first text byte with the base page at 4(sp).
| Programs are built with 16-bit ints: a gate's function number is a
| word at 4(sp), its parameters longs after it.

	.text
	.globl	_start, __bdos, __bdosl, __bios

_start:
	movel	%sp@(4),%sp@-
	jsr	_cstart
	clrl	%d0
	trap	&2

| __bdos(fn, param): the result in d0
__bdos:
__bdosl:
	movew	%sp@(4),%d0
	movel	%sp@(6),%d1
	trap	&2
	rts

| __bios(fn, p1, p2)
__bios:
	movel	%d2,%sp@-
	movew	%sp@(8),%d0
	movel	%sp@(10),%d1
	movel	%sp@(14),%d2
	trap	&3
	movel	%sp@+,%d2
	rts
