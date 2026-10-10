/*
 * GSX-86 calls, INT 0E0h with CX = 0473h: invoke the driver in layer 0.
 * DS:DX holds five far pointers, offset then segment:
 *
 *	+0 contrl  +4 intin  +8 ptsin  +12 intout  +16 ptsout
 *
 * contrl[1] and [3] count the ptsin vertices and intin words; the driver
 * sets contrl[2] and [4] for ptsout and intout.  The caller gets the
 * driver's registers back, and this driver leaves them alone.
 */

#include "i86.h"
#include "gsx.h"

#define NCTRL	6		/* contrl words a driver reads		*/
#define NIN	128		/* intin words				*/
#define NOUT	45		/* intout words, the Open Workstation table */
#define NPOUT	12		/* ptsout words				*/

static int	ctrl[NCTRL], in[NIN], pin[2 * GSXMAXV];
static int	out[NOUT], pout[NPOUT];
static int	bound;		/* a workstation is open		*/

/*
 * The device: (m, pb) draws one call.  Null, or nonzero from Open
 * Workstation, means there is none.  A host test points it at a
 * surface in guest memory.
 */
int	(*i86gsxdev)();

static char *hex4(p, v)
char *p;
unsigned v;
{
	int i;

	for (i = 12; i >= 0; i -= 4)
		*p++ = "0123456789ABCDEF"[(v >> i) & 15];
	return (p);
}

static char *str(p, s)
char *p, *s;
{
	while (*s)
		*p++ = *s++;
	return (p);
}

/* No driver bound: GDOS names the caller and the layer, unloads itself
 * and ends the program. */
static int nodev(m)
struct i86 *m;
{
	char buf[64], *p;

	p = str(buf, "GSX-86 ");
	p = hex4(p, m->sr[S_CS]);
	*p++ = ':';
	p = hex4(p, m->ip);
	p = str(p, " Invalid Gios0000h (hex)\n\rGSX Removed\r\n$");
	i86sys(9, (i16)0, buf);
	return (B_EXIT);
}

/* Host address of n words at the far pointer in pb[k], or 0. */
static char *farp(m, pb, k, n)
struct i86 *m;
char *pb;
int k, n;
{
	char *q;

	q = pb + 4 * k;
	return (i86far(m, (i16)((q[2] & 0xff) | (q[3] & 0xff) << 8),
	    (i16)((q[0] & 0xff) | (q[1] & 0xff) << 8), (i32)(n ? 2 * n : 1)));
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

int i86gsx(m)
struct i86 *m;
{
	register int i;
	char *pb, *pc, *pi, *pp;
	struct gsxpb g;

	if ((m->r[R_CX] & 0xff00) != 0x0400)
		return (B_FN);
	if ((pb = i86addr(m, S_DS, m->r[R_DX], 20L)) == (char *)0
	    || (pc = farp(m, pb, 0, NCTRL)) == (char *)0)
		return (B_ADDR);
	for (i = 0; i < NCTRL; i++)
		ctrl[i] = rdw(pc + 2 * i);
	if (ctrl[0] != 1 && !bound)
		return (nodev(m));

	/* more than the device takes: nothing drawn, nothing returned */
	if ((unsigned)ctrl[1] > GSXMAXV || (unsigned)ctrl[3] > NIN) {
		wrw(pc + 4, 0);
		wrw(pc + 8, 0);
		return (B_RUN);
	}
	pi = farp(m, pb, 1, ctrl[3]);
	pp = farp(m, pb, 2, 2 * ctrl[1]);
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
	if (ctrl[0] == 1) {
		bound = i86gsxdev && (*i86gsxdev)(m, &g) == 0;
		if (!bound)
			return (nodev(m));
	} else
		(*i86gsxdev)(m, &g);

	if (ctrl[4] > NOUT)
		ctrl[4] = NOUT;
	if (ctrl[2] > NPOUT / 2)
		ctrl[2] = NPOUT / 2;
	pi = farp(m, pb, 3, ctrl[4]);
	pp = farp(m, pb, 4, 2 * ctrl[2]);
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
