/*
 * amigarom.c -- the Amiga host's Kickstart, for the Amiga environment.
 *
 * Kickstart sits at $F80000; the kernel reaches it one to one through
 * the transparent window over the low gigabyte.  The environment module
 * checks the contents.
 */

/* kernel address of the first n bytes of Kickstart, 0 beyond it */
unsigned long
amiga_romva(n)
unsigned long n;
{
	return n <= 0x80000 ? 0xf80000 : 0;
}
