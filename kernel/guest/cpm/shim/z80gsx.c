/*
 * GSX-80 calls, BDOS function 115.  DE holds five guest addresses:
 *
 *	+0 contrl  +2 intin  +4 ptsin  +6 intout  +8 ptsout
 *
 * contrl[1] and [3] count the ptsin vertices and intin words; the device
 * sets contrl[2] and [4] for ptsout and intout.  A call the device
 * refuses returns no outputs.
 */

#include "z80.h"
#include "gsx.h"

#define NCTRL	6		/* contrl words a driver reads		*/
#define NIN	128		/* intin words				*/
#define NOUT	45		/* intout words, the Open Workstation table */
#define NPOUT	12		/* ptsout words				*/

static int	ctrl[NCTRL], in[NIN], pin[2 * GSXMAXV];
static int	out[NOUT], pout[NPOUT];

/* Host address of n words at the guest address in pb[k], or 0. */
static char *adr(m, pb, k, n)
struct z80 *m;
char *pb;
int k, n;
{
	pb += 2 * k;
	return (z80addr(m, (z16)((pb[0] & 0xff) | (pb[1] & 0xff) << 8),
	    (z32)(n ? 2 * n : 1)));
}

static int rdw(p)
char *p;
{
	return ((short)((p[0] & 0xff) | (p[1] & 0xff) << 8));
}

static void wrw(p, v)
char *p;
int v;
{
	p[0] = (char)v;
	p[1] = (char)(v >> 8);
}

int z80gsx(m, de)
struct z80 *m;
z16 de;
{
	register int i;
	char *pb, *pc, *pi, *pp;
	struct gsxpb g;

	if ((pb = z80addr(m, de, (z32)10)) == (char *)0
	    || (pc = adr(m, pb, 0, NCTRL)) == (char *)0)
		return (B_ADDR);
	for (i = 0; i < NCTRL; i++)
		ctrl[i] = rdw(pc + 2 * i);

	/* more than the device takes: nothing drawn, nothing returned */
	if ((unsigned)ctrl[1] > GSXMAXV || (unsigned)ctrl[3] > NIN) {
		wrw(pc + 4, 0);
		wrw(pc + 8, 0);
		return (B_RUN);
	}
	pi = adr(m, pb, 1, ctrl[3]);
	pp = adr(m, pb, 2, 2 * ctrl[1]);
	if (pi == (char *)0 || pp == (char *)0)
		return (B_ADDR);
	for (i = 0; i < NIN; i++)
		in[i] = i < ctrl[3] ? rdw(pi + 2 * i) : 0;
	for (i = 0; i < 2 * ctrl[1]; i++)
		pin[i] = rdw(pp + 2 * i);

	g.contrl = ctrl;
	g.intin = in;
	g.ptsin = pin;
	g.intout = out;
	g.ptsout = pout;
	ctrl[2] = ctrl[4] = 0;
	if (z80gsxd(m, &g) != 0)
		ctrl[2] = ctrl[4] = 0;

	if (ctrl[4] > NOUT)
		ctrl[4] = NOUT;
	if (ctrl[2] > NPOUT / 2)
		ctrl[2] = NPOUT / 2;
	pi = adr(m, pb, 3, ctrl[4]);
	pp = adr(m, pb, 4, 2 * ctrl[2]);
	if (pi == (char *)0 || pp == (char *)0)
		return (B_ADDR);
	for (i = 0; i < ctrl[4]; i++)
		wrw(pi + 2 * i, out[i]);
	for (i = 0; i < 2 * ctrl[2]; i++)
		wrw(pp + 2 * i, pout[i]);
	wrw(pc + 4, ctrl[2]);
	wrw(pc + 8, ctrl[4]);
	return (B_RUN);
}
