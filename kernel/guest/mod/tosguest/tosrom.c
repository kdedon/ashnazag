/*
 * tosrom.c -- the machine's own TOS ROM, mapped in place into the
 * launcher by TOSIOC_MAPROM.
 *
 * The host platform's hook romva(n) gives the kernel address of the
 * first n bytes of its ROM, 0 if it has none or n is beyond it; an
 * Atari host has ata_romva.  The ROM is checked once: a TOS 3 or 4
 * header (bra, version $03xx-$04xx, base $E00000, reset PC inside).
 * Its size is the largest the hook accepts: 1 MB (a CT60's flash) or
 * 512 KB.  It is mapped in place, read-only: nothing patches it.
 * Without a usable ROM the ioctl fails and the launcher loads EmuTOS
 * from its file.
 *
 * K&R C.
 */

#include "tos.h"
__asm__(".weak ata_romva");
extern unsigned long ata_romva();
/* read at run time: the compiler takes a declared function's address as nonzero */
static unsigned long (*volatile romva)() = ata_romva;

static unsigned long romsize;	/* 0 not checked, 1 none, else the size */

/* the host's ROM size after the checks, 0 if unusable */
static unsigned long
hwrom()
{
	unsigned char *b;
	unsigned long v, pc;

	if (romsize)
		return romsize > 1 ? romsize : 0;
	romsize = 1;
	if (romva == 0 || (b = (unsigned char *)(*romva)(0x80000L)) == 0)
		return 0;
	v = G16(b + 2);
	pc = G32(b + 4);
	if ((G16(b) & 0xff00) != 0x6000 || v < 0x300 || v > 0x4ff ||
	    G32(b + 8) != TOS_ROMBASE || pc < TOS_ROMBASE || pc >= TOS_ROMBASE + 0x80000) {
		printf("tosguest: no TOS 3 or 4 ROM at %x\n", (int)TOS_ROMBASE);
		return 0;
	}
	romsize = (*romva)(0x100000L) ? 0x100000L : 0x80000L;
	printf("tosguest: ROM TOS %x, %d KB\n", (int)v, (int)(romsize >> 10));
	return romsize;
}

/* the ROM at TOS_ROMBASE in the caller, through its open dev; size in *rvp */
int
tos_maprom(dev, rvp)
	dev_t dev;
	int *rvp;
{
	unsigned long n;
	int e;

	if ((n = hwrom()) == 0)
		return ENODEV;
	if ((e = guest_rommap(dev, TOS_ROMBASE, n)) == 0)
		*rvp = (int)n;
	return e;
}
