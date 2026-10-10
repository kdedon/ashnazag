| segclra.s -- zero the 64 KB segment at p, as segclr.c does.
|
|	int segclr(p)  char *p;

	.text
	.globl	segclr
segclr:
	movel	%sp@(4),%a0
	moveq	&0,%d0
	movew	&0x0fff,%d1		| 4096 times 16 bytes
1:	movel	%d0,%a0@+
	movel	%d0,%a0@+
	movel	%d0,%a0@+
	movel	%d0,%a0@+
	dbra	%d1,1b
	rts
