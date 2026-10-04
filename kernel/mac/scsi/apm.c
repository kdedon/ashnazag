/*
 * Apple Partition Map scan: map entries to AMIX slices 1..7.
 *
 * Built into the kernel and into the host test, so it uses no kernel
 * headers and reads the big-endian map bytewise.
 */

#include "apm.h"

#define be16(p)	((unsigned)(p)[0] << 8 | (p)[1])
#define be32(p)	((unsigned long)(p)[0] << 24 | (unsigned long)(p)[1] << 16 \
		| (unsigned long)(p)[2] << 8 | (p)[3])

/* entry classes */
#define C_SKIP	0
#define C_UNIX	1
#define C_OTHER	2

struct ent {
	unsigned long	base, len;
	unsigned short	flags;		/* bzb flags, 0 without bzb */
	char		bzb;		/* bzb magic present */
	char		bzbtype;
	char		class;
	char		used;
	char		name[32];
	char		type[32];
};

static struct ent	ents[APM_MAXENT];
static unsigned char	blk[APM_BSIZE];

static int
lc(c)
int c;
{
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* case-insensitive compare of s with the first strlen(t) bytes, or all of it */
static int
teq(s, t, prefix)
char *s, *t;
int prefix;
{
	while (*t)
		if (lc(*s++) != lc(*t++))
			return 0;
	return prefix || *s == 0;
}

/* case-insensitive substring */
static int
tsub(s, t)
char *s, *t;
{
	char *a, *b;

	for (; *s; s++) {
		for (a = s, b = t; *b && lc(*a) == lc(*b); a++, b++)
			;
		if (*b == 0)
			return 1;
	}
	return 0;
}

static void
cpstr(d, s)
char *d;
unsigned char *s;
{
	int i;

	for (i = 0; i < 31 && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
}

static int
classify(t)
char *t;
{
	if (teq(t, "Apple_UNIX_SVR2", 0))
		return C_UNIX;
	if (teq(t, "Apple_partition_map", 0) || teq(t, "Apple_Driver", 1)
	|| teq(t, "Apple_FWDriver", 1) || teq(t, "Apple_Free", 0)
	|| teq(t, "Apple_Void", 0) || teq(t, "Apple_Patches", 0)
	|| t[0] == 0)
		return C_SKIP;
	return C_OTHER;
}

static void
take(sl, s, e, i, how)
struct apm_slice *sl;
struct ent *e;
int s, i, how;
{
	char *a, *b;

	sl[s].base = e->base;
	sl[s].len = e->len;
	sl[s].entry = i + 1;
	sl[s].how = how;
	for (a = sl[s].name, b = e->name; *a++ = *b++; )
		;
	for (a = sl[s].type, b = e->type; *a++ = *b++; )
		;
	e->used = 1;
}

static int
lowfree(sl)
struct apm_slice *sl;
{
	int s;

	for (s = 4; s < APM_NSLICE; s++)
		if (sl[s].how == APM_NONE)
			return s;
	return 0;
}

int
apm_scan(rd, arg, sl)
int (*rd)();
char *arg;
struct apm_slice *sl;
{
	struct ent *e;
	unsigned char *b;
	unsigned long n;
	int i, s, err, bsize;

	for (s = 0; s < APM_NSLICE; s++) {
		sl[s].base = sl[s].len = 0;
		sl[s].entry = 0;
		sl[s].how = APM_NONE;
		sl[s].name[0] = sl[s].type[0] = 0;
	}

	if (err = (*rd)(arg, 0L, blk))
		return err;
	if (be16(blk) == APM_DDM_SIG) {
		bsize = be16(blk + 2);
		if (bsize != 0 && bsize != APM_BSIZE)
			return -1;
		sl[0].len = be32(blk + 4);
	}
	sl[0].how = APM_WHOLE;

	n = 1;
	for (i = 0; i < n && i < APM_MAXENT; i++) {
		e = &ents[i];
		e->class = C_SKIP;
		e->used = 0;
		if (err = (*rd)(arg, (long)i + 1, blk))
			return err;
		b = blk;
		if (be16(b + PM_SIG) != APM_PM_SIG) {
			if (i == 0)
				return -1;
			continue;
		}
		if (i == 0)
			n = be32(b + PM_MAPCNT);
		e->base = be32(b + PM_START);
		e->len = be32(b + PM_COUNT);
		cpstr(e->name, b + PM_NAME);
		cpstr(e->type, b + PM_TYPE);
		e->bzb = be32(b + PM_BZB + BZB_MAGIC) == APM_BZB_MAGIC;
		e->bzbtype = e->bzb ? b[PM_BZB + BZB_TYPE] : 0;
		e->flags = e->bzb ? be16(b + PM_BZB + BZB_FLAGS) : 0;
		if (e->len != 0)
			e->class = classify(e->type);
	}
	n = i;

	/* explicit A/UX slice numbers */
	for (i = 0, e = ents; i < n; i++, e++) {
		s = e->flags & BZB_SLICE;
		if (e->class == C_UNIX && s >= 1 && s < APM_NSLICE
		&& sl[s].how == APM_NONE)
			take(sl, s, e, i, APM_BZBSLICE);
	}
	/* A/UX roles from the bzb: root, swap, usr */
	for (i = 0, e = ents; i < n; i++, e++) {
		if (e->class != C_UNIX || e->used || !e->bzb)
			continue;
		if (e->flags & BZB_ROOT)
			s = 1;
		else if (e->bzbtype == BZB_FSTSFS)
			s = 2;
		else if (e->flags & BZB_USR)
			s = 3;
		else
			continue;
		if (sl[s].how == APM_NONE)
			take(sl, s, e, i, APM_BZBROLE);
	}
	/* SVR2 entries without a bzb: roles by name */
	for (i = 0, e = ents; i < n; i++, e++) {
		if (e->class != C_UNIX || e->used || e->bzb)
			continue;
		if (tsub(e->name, "root"))
			s = 1;
		else if (tsub(e->name, "swap"))
			s = 2;
		else if (tsub(e->name, "usr"))
			s = 3;
		else
			continue;
		if (sl[s].how == APM_NONE)
			take(sl, s, e, i, APM_NAMEROLE);
	}
	/* the rest in map order: SVR2 first, then other data partitions */
	for (i = 0, e = ents; i < n; i++, e++)
		if (e->class == C_UNIX && !e->used && (s = lowfree(sl)))
			take(sl, s, e, i, APM_UNIX);
	for (i = 0, e = ents; i < n; i++, e++)
		if (e->class == C_OTHER && !e->used && (s = lowfree(sl)))
			take(sl, s, e, i, APM_OTHER);
	return 0;
}
