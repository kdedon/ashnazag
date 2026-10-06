/*
 * amigarom.c -- the machine's own Kickstart, mapped in place into the
 * launcher by AMIGAIOC_MAPROM.
 *
 * The host platform's hook amiga_romva(n) gives the kernel address of
 * the first n bytes of its Kickstart, 0 if it has none or n is beyond
 * it.  Checked once: 512 KB at $F80000, the $1111 or $1114 header with
 * a jmp into the ROM, and the ROM's sum ($FFFFFFFF, carries added back).
 * Mapped read-only: nothing patches it.  Without a usable Kickstart the
 * ioctl fails and the launcher loads the user's file.
 *
 * K&R C.
 */

#include "amiga.h"

__asm__(".weak amiga_romva");
extern unsigned long amiga_romva();
/* read at run time: the compiler takes a declared function's address as nonzero */
static unsigned long (*volatile romva)() = amiga_romva;

static int romstate;		/* 0 not checked, 1 usable, -1 not */

static int
hwrom()
{
	unsigned char *b;
	unsigned long sum = 0, prev, i, pc;

	if (romstate)
		return romstate > 0;
	romstate = -1;
	if (romva == 0 || (b = (unsigned char *)(*romva)(AMIGA_ROM_SIZE)) == 0)
		return 0;
	pc = G32(b + 4);
	if ((G16(b) != 0x1111 && G16(b) != 0x1114) || G16(b + 2) != 0x4ef9 ||
	    pc < AMIGA_ROM_BASE || pc >= AMIGA_ROM_BASE + AMIGA_ROM_SIZE) {
		printf("amigaguest: no Kickstart at %x\n", (int)AMIGA_ROM_BASE);
		return 0;
	}
	for (i = 0; i < AMIGA_ROM_SIZE; i += 4) {
		prev = sum;
		sum += G32(b + i);
		if (sum < prev)
			sum++;
	}
	if (sum != 0xffffffffUL) {
		printf("amigaguest: Kickstart sum %x\n", (int)sum);
		return 0;
	}
	printf("amigaguest: Kickstart %d.%d\n", (int)G16(b + 12), (int)G16(b + 14));
	romstate = 1;
	return 1;
}

/* the Kickstart at AMIGA_ROM_BASE in the caller, through its open dev */
int
amiga_maprom(dev, rvp)
	dev_t dev;
	int *rvp;
{
	int e;

	if (!hwrom())
		return ENODEV;
	if ((e = guest_rommap(dev, AMIGA_ROM_BASE, AMIGA_ROM_SIZE)) == 0)
		*rvp = (int)AMIGA_ROM_SIZE;
	return e;
}
