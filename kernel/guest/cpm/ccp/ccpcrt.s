| ccpcrt.s -- start of the transient CCP and its BDOS gate.
|
| main runs one command line; its state outlives the CCP's reload
| through functions 150 and 151.

	.text
	.globl	_start, bdos

_start:
	jsr	main
	bra.b	_start

| bdos(fn, param), 16-bit fn
bdos:
	movew	%sp@(4),%d0
	movel	%sp@(6),%d1
	trap	&2
	rts
