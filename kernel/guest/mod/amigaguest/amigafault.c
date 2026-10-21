#include "amiga.h"

static int amiga_nlog;	/* hardware accesses reported */

static void
site(c, pc, a)
	struct amigacensus *c;
	unsigned long pc, a;
{
	int i, k;
	for (i = 0; i < 64 && c->site[i][2]; i++)
		if (c->site[i][0] == pc && c->site[i][1] == a) break;
	/* full: the least counted gives way, so the busiest stay */
	if (i == 64)
		for (k = i = 0; k < 64; k++)
			if (c->site[k][2] < c->site[i][2]) i = k;
	c->site[i][0] = pc;
	c->site[i][1] = a;
	c->site[i][2]++;
}

/*
 * A bus error outside the constant pages.  Guest RAM and the host's
 * windows above the 24-bit space are paged in by the kernel; the rest is
 * hardware nothing answers: counted, the first few logged, and left to
 * the kernel as a bus error.
 */
int
amiga_fault(gp, r, v)
	struct guest_proc *gp;
	char *r;
	int v;
{
	struct amigactr *a = AMIGAP(gp);
	unsigned long fa;
	int ssw, fmt = GR_FV(r) >> 12;

	if ((fmt != 7 && fmt != 4) || guest_bfault(r, &fa, &ssw) != 0)
		return 1;
	if (fa < a->ac_config.ae_chipsize || (fa >= 0x1000000UL && fa < 0xff000000UL))
		return 1;
	a->ac_stat.as_fault++;
	a->ac_stat.as_lastpc = GR_PC(r);
	a->ac_stat.as_lastaddr = fa;
	if (!a->ac_stat.as_bad++) {
		a->ac_stat.as_badpc = GR_PC(r);
		a->ac_stat.as_badaddr = fa;
	}
	if (a->ac_config.ae_flags & AMIGAF_CENSUS)
		site(&a->ac_census, (unsigned long)GR_PC(r), fa);
	if (amiga_nlog < 8) {
		amiga_nlog++;
		printf("amiga: hardware access at pc %x, address %x\n", (int)GR_PC(r), (int)fa);
	}
	return 1;
}

/*
 * With the whole patch set the register pages and the absent windows the
 * Kickstart probes are constant pages: reads are memory, writes reach
 * amiga_access.  Values are those of an idle machine.
 */
static unsigned char cp_custom[0x200], cp_ciaa[0xf02], cp_ciab[0xf01], cp_gary[0x44];

static void
cp_put16(b, o, v)
	unsigned char *b;
	int o;
	unsigned long v;
{
	b[o] = v >> 8;
	b[o + 1] = v;
}

static void
cp_init()
{
	int i;

	cp_put16(cp_custom, 0x002, 0x2000UL);	/* DMACONR: blitter idle */
	cp_put16(cp_custom, 0x004, 0x2200UL);	/* VPOSR: PAL, line 0 */
	cp_put16(cp_custom, 0x012, 0xffffUL);
	cp_put16(cp_custom, 0x014, 0xffffUL);
	cp_put16(cp_custom, 0x016, 0xff00UL);	/* POTGOR: buttons up */
	cp_put16(cp_custom, 0x018, 0x3000UL);	/* SERDATR: transmitter empty */
	cp_put16(cp_custom, 0x07c, 0x00f8UL);	/* DENISEID */
	/* ports high (no buttons, drives deselected and absent), timers at their latch */
	for (i = 0; i < 16; i++)
		cp_ciaa[0x001 + i * 0x100] = cp_ciab[i * 0x100] =
		    i < 2 || (i >= 4 && i < 8) ? 0xff : 0;
	for (i = 0; i < 0xf01; i++)
		if (i & 1)
			cp_ciab[i] = 0xff;
		else if (i > 0)
			cp_ciaa[i] = 0xff;
	cp_ciaa[0] = 0xff;
	cp_gary[0x43] = 0x7f;
}

static struct { unsigned long va, len; unsigned char *data; unsigned long dlen; int fill; } cp_tab[] = {
	{ 0xdff000UL, 0x1000, cp_custom, sizeof cp_custom, 0 },
	{ 0xbfe000UL, 0x1000, cp_ciaa, sizeof cp_ciaa, 0xff },
	{ 0xbfd000UL, 0x1000, cp_ciab, sizeof cp_ciab, 0xff },
	{ 0xde0000UL, 0x2000, cp_gary, sizeof cp_gary, 0 },
	/* chip RAM's end, slow RAM, Zorro II configuration, clock, IDE, the board's fast RAM and ROM */
	{ AMIGA_CHIP_SIZE, 0x1000, 0, 0, 0xff },
	{ 0xa00000UL, 0x1000, 0, 0, 0xff },
	{ 0xc3f000UL, 0x1000, 0, 0, 0xff },
	{ 0xdc0000UL, 0x1000, 0, 0, 0xff },
	{ 0xdd0000UL, 0x4000, 0, 0, 0xff },
	{ 0xe80000UL, 0x10000, 0, 0, 0xff },
	{ 0x7f00000UL, 0x1000, 0, 0, 0xff },
	{ 0xff000000UL, 0x10000, 0, 0, 0xff }
};

/* the constant pages, for a session with every patch; errno */
int
amiga_cpages(s)
	struct guest_sess *s;
{
	struct amigactr *a = AMIGAP(s->gs_gp);
	struct guest_cpage *cp = a->ac_cpage;
	int i, n = 0;

	if (!cp_custom[0x7d])
		cp_init();
	bcopy((caddr_t)cp_custom, (caddr_t)a->ac_custom, sizeof cp_custom);
	a->ac_cpreq = a->ac_cpena = 0;
	for (i = 0; i < sizeof cp_tab / sizeof cp_tab[0] && n < AMIGA_NCPAGE; i++) {
		cp[n].cp_va = cp_tab[i].va;
		cp[n].cp_len = cp_tab[i].len;
		cp[n].cp_data = i ? cp_tab[i].data : a->ac_custom;
		cp[n].cp_dlen = cp_tab[i].dlen;
		cp[n].cp_fill = cp_tab[i].fill;
		cp[n].cp_id = i;
		n++;
	}
	/* where fast RAM ends, which the memory probe reads */
	cp[n].cp_va = AMIGA_FAST_BASE + a->ac_config.ae_fastsize;
	cp[n].cp_len = 0x1000;
	cp[n].cp_data = 0;
	cp[n].cp_dlen = 0;
	cp[n].cp_fill = 0xff;
	cp[n].cp_id = i;
	n++;
	return guest_cpage_map(s, cp, n);
}

/* a write to a constant page: interrupt words and ICR masks to the host's state, the rest dropped */
int
amiga_access(s, r, cp, ga)
	struct guest_sess *s;
	char *r;
	struct guest_cpage *cp;
	struct guest_access *ga;
{
	struct guest_proc *gp = s->gs_gp;
	struct amigactr *a = AMIGAP(gp);
	unsigned long ad = ga->ga_addr, v = ga->ga_val, w;
	unsigned short *p;
	int sz = ga->ga_size, m, nm, spl, k;

	a->ac_stat.as_fault++;
	a->ac_stat.as_hw++;
	a->ac_stat.as_lastpc = GR_PC(r);
	a->ac_stat.as_lastaddr = ad;
	if (a->ac_config.ae_flags & AMIGAF_CENSUS)
		site(&a->ac_census, (unsigned long)GR_PC(r), ad);
	if (cp->cp_va != 0xdff000UL)
		a->ac_stat.as_cia++;
	/* an ICR write sets (bit 7) or clears mask bits, kept with the guest's in memory */
	k = ad <= 0xbfed01UL && ad + sz > 0xbfed01UL ? 0 : ad <= 0xbfdd00UL && ad + sz > 0xbfdd00UL ? 1 : -1;
	if (k >= 0 || (ad & ~1UL) == 0xdff09cUL)
		a->ac_stat.as_intreg++;
	if (k >= 0) {
		unsigned char b;
		caddr_t pv = (caddr_t)AMIGA_PV_BASE + 16 + k;
		w = v >> 8 * (ad + sz - 1 - (k ? 0xbfdd00UL : 0xbfed01UL)) & 0xff;
		if (copyin(pv, (caddr_t)&b, 1) == 0) {
			b = w & 0x80 ? b | (w & 0x7f) : b & ~w;
			(void)copyout((caddr_t)&b, pv, 1);
		}
		amiga_pvpull(gp);
		amiga_pvpush(gp);
		amiga_repost(gp);
		return 0;
	}
	if (!sz || cp->cp_va != 0xdff000UL)
		return 0;
	amiga_pvpull(gp);
	m = amiga_pvget(gp);
	spl = guest_splhi();
	if (m >= 0)
		a->ac_intena = (a->ac_intena & ~0x4000) | m;
	/* INTENA and INTREQ: bit 15 sets the others, else clears them */
	for (k = 0; k + 2 <= sz; k += 2) {
		w = v >> 8 * (sz - 2 - k) & 0xffff;
		if (((ad + k) & 0x1fe) == 0x09a) {
			p = &a->ac_intena;
			a->ac_stat.as_intena++;
		} else if (((ad + k) & 0x1fe) == 0x09c)
			p = &a->ac_intreq;
		else
			continue;
		*p = w & 0x8000 ? *p | (w & 0x7fff) : *p & ~w;
	}
	nm = a->ac_intena & 0x4000;
	a->ac_epoch++;
	guest_splx(spl);
	if (m >= 0 && nm != m)
		amiga_pvput(gp, nm);
	amiga_pvpush(gp);
	amiga_repost(gp);
	amiga_pvclock(gp);
	return 0;
}
