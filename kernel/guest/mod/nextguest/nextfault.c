/*
 * nextfault.c -- access faults of the guest.
 *
 * Every fault goes to the guest's own handler.  One at an address nothing
 * backs (no RAM, ROM, VRAM or window there) is the bus error the hardware
 * would give, and is counted.
 *
 * K&R C.
 */

#include "next.h"
#include "../../next/hfswire.h"

static struct {
	unsigned long a, n;
} windows[] = NX_WINDOWS;

/* 1: nothing backs a on the local bus, 0: memory */
int
nx_dev(a, wr)
	unsigned long a;
	int wr;
{
	int i;

	for (i = 0; i < sizeof windows / sizeof windows[0]; i++)
		if (a - windows[i].a < windows[i].n)
			return 0;
	if (a >= NX_LIMIT)
		return (a & 0xffff0000UL) == 0x820c0000UL;
	if (a < NX_ROM + NX_ROM_SIZE || (a >= NX_ROM2 && a < NX_ROM2 + NX_ROM_SIZE))
		return wr;
	if (a >= NX_RAM && a < NX_RAM + nx.ramsize)
		return 0;
	if (a >= NX_VRAM && a < NX_VRAM + NX_VRAM_SIZE)
		return 0;
	if (a >= HF_BASE && a < HF_BASE + HF_SIZE)
		return 0;
	return 1;
}

/* the guest physical address of a; one that does not translate is memory, so its fault reaches the guest */
static unsigned long
phys(a, wr)
	unsigned long a;
	int wr;
{
	unsigned long pa;

	if (!nx.gp)
		return a;
	return guest_mmu_xlate(nx.gp, a, wr ? 2 : 1, &pa) ? NX_RAM : pa;
}

/* 0: done, 1: not ours (the stock handler) */
int
nx_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	unsigned long fa;
	int ssw, wr;

	/* the host's own code, such as its signal handlers, keeps its signals */
	if ((nx.flags & NXF_HOST) && (unsigned long)GR_PC(r) >= NX_TOP)
		return 1;
	if (guest_bfault(r, &fa, &ssw) >= 0) {
		wr = !(ssw & 0x100);
		/* guest accesses to the host's space, counted for the test */
		if (fa >= NX_TOP) {
			nx.st.high++;
			nx.st.higha = fa;
			nx.st.highpc = GR_PC(r);
		}
		if (nx_dev(phys(fa, wr), wr)) {
			nx.st.io++;
			if ((nx.flags & NXF_CENSUS) && nx_census(fa, wr))
				printf("NOTICE: next: nothing at %x pc %x\n", (int)fa, (int)GR_PC(r));
		}
	}
	return guest_fnote(gp, r, v);
}
