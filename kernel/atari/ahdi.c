/*
 * AHDI partition table scan (see ahdi.h).
 */

#include "ahdi.h"

#define XGM_MAX		32		/* extended partitions followed */

static unsigned long
be32(p)
unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

static int
idis(p, s)
unsigned char *p;
char *s;
{
	return p[0] == s[0] && p[1] == s[1] && p[2] == s[2];
}

static int
idok(p)
unsigned char *p;
{
	int i;

	for (i = 0; i < 3; i++)
		if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9')))
			return 0;
	return 1;
}

static void
put(sl, s, e, base)
struct ahdi_slice *sl;
int s;
unsigned char *e;
unsigned long base;
{
	sl[s].base = base + be32(e + AP_ST);
	sl[s].len = be32(e + AP_SIZ);
	sl[s].id[0] = e[AP_ID];
	sl[s].id[1] = e[AP_ID + 1];
	sl[s].id[2] = e[AP_ID + 2];
	sl[s].id[3] = 0;
}

/* GEM and BGM go to the first free slice from 4 on. */
static void
other(sl, e, base)
struct ahdi_slice *sl;
unsigned char *e;
unsigned long base;
{
	int s;

	if (!idis(e + AP_ID, "GEM") && !idis(e + AP_ID, "BGM"))
		return;
	for (s = 4; s < AHDI_NSLICE; s++)
		if (sl[s].len == 0) {
			put(sl, s, e, base);
			return;
		}
}

/* blocks [a, a+m) and [b, b+n) share one */
static int
overlap(a, m, b, n)
unsigned long a, m, b, n;
{
	return a < b ? b - a < m : a - b < n;
}

int
ahdi_scan(rd, arg, sl)
int (*rd)();
char *arg;
struct ahdi_slice *sl;
{
	unsigned char b[AHDI_BSIZE], *e;
	unsigned long xgm, next, cur, st[5], len[5], hd;
	int i, n, s, err, own[4];

	for (i = 0; i < AHDI_NSLICE; i++) {
		sl[i].base = sl[i].len = 0;
		sl[i].id[0] = 0;
	}
	if ((err = (*rd)(arg, 0L, b)) != 0)
		return err;
	n = 0;
	xgm = 0;
	for (i = 0; i < 4; i++) {
		e = b + AHDI_PART + i * AHDI_PSIZE;
		st[i] = be32(e + AP_ST);
		len[i] = e[AP_FLG] & 1 ? be32(e + AP_SIZ) : 0;
		if (!(e[AP_FLG] & 1) || !idok(e + AP_ID) || be32(e + AP_SIZ) == 0)
			continue;
		n++;
		if (idis(e + AP_ID, "AXR") && sl[1].len == 0) {
			put(sl, 1, e, 0L);
			own[1] = i;
		} else if (idis(e + AP_ID, "AXS") && sl[2].len == 0) {
			put(sl, 2, e, 0L);
			own[2] = i;
		} else if (idis(e + AP_ID, "AXU") && sl[3].len == 0) {
			put(sl, 3, e, 0L);
			own[3] = i;
		} else if (idis(e + AP_ID, "XGM") && xgm == 0)
			xgm = be32(e + AP_ST);
		else
			other(sl, e, 0L);
	}
	if (n == 0)
		return -1;
	sl[0].len = hd = be32(b + AHDI_HDSIZ);
	sl[0].id[0] = 0;

	/*
	 * Drop an AX slice that leaves the disk or meets the root sector,
	 * the bad sector list or any other partition.
	 */
	st[4] = be32(b + AHDI_BSLST);
	len[4] = be32(b + AHDI_BSLCNT);
	for (s = 1; s < 4; s++) {
		if (sl[s].len == 0)
			continue;
		if (sl[s].base == 0 || (hd && (sl[s].base >= hd || sl[s].len > hd - sl[s].base)))
			sl[s].len = 0;
		for (i = 0; i < 5; i++)
			if (len[i] && i != own[s]
			&& overlap(sl[s].base, sl[s].len, st[i], len[i]))
				sl[s].len = 0;
	}

	/*
	 * Each extended root sector: one partition, then the link to the
	 * next, which lies further on.
	 */
	for (next = xgm, i = 0; next != 0 && i < XGM_MAX; i++) {
		if ((*rd)(arg, next, b) != 0)
			break;
		e = b + AHDI_PART;
		if (e[AP_FLG] & 1)
			other(sl, e, next);
		e += AHDI_PSIZE;
		cur = next;
		next = (e[AP_FLG] & 1) && idis(e + AP_ID, "XGM") ?
		    xgm + be32(e + AP_ST) : 0;
		if (next <= cur)
			break;
	}
	return 0;
}
