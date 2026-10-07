/*
 * amdos.c -- the dos.library (and the one intuition call) a library
 * needs to read its configuration and load its plugins.
 *
 * Files are read-only and come whole from one host directory
 * (am_confdir, e.g. /etc/conf/pci).  Every AmigaOS name, with or without
 * a volume ("ENVARC:PCI-Configuration", "LIBS:PCI", "SetFoo"), means
 * the file named by its last component in that directory; the last
 * component of a directory-ish name ("LIBS:PCI") is the directory
 * itself when no such file exists.  So nothing outside the directory
 * can be named.
 *
 * ReadArgs follows AmigaDOS: aliases (A=B), /A /K /S /N /T /M /F, KEY=value,
 * quoted strings with * escapes, /M giving back its last items to later
 * /A arguments.  What it allocates hangs off RDA_DAList until FreeArgs.
 *
 * intuition.library has DisplayAlert only: the alert's text lines go to
 * the console, and a recoverable alert answers 0.
 */

#include "amilib.h"

#define	AM_MAXFILE	(256L * 1024)
#define	AM_MAXITEMS	32

/* dos/dos.h */
#define	MODE_OLDFILE	1005
#define	MODE_NEWFILE	1006
#define	MODE_READWRITE	1004
#define	OFFSET_BEGINNING (-1)
#define	OFFSET_CURRENT	0
#define	OFFSET_END	1
#define	DOS_FIB		2
#define	DOS_RDARGS	5
#define	FIB_SIZE	260
#define	ERROR_NO_FREE_STORE		103
#define	ERROR_BAD_TEMPLATE		114
#define	ERROR_BAD_NUMBER		115
#define	ERROR_REQUIRED_ARG_MISSING	116
#define	ERROR_KEY_NEEDS_ARG		117
#define	ERROR_TOO_MANY_ARGS		118
#define	ERROR_UNMATCHED_QUOTES		119
#define	ERROR_OBJECT_NOT_FOUND		205
#define	ERROR_OBJECT_WRONG_TYPE		212
#define	ERROR_WRITE_PROTECTED		214
#define	ERROR_SEEK_ERROR		219

/* dos/rdargs.h */
#define	CS_BUFFER	0
#define	CS_LENGTH	4
#define	CS_CURCHR	8
#define	RDA_DALIST	12
#define	RDA_SIZE	32

char am_confdir[64];

/* a file handle or lock: ours, behind a BPTR */
struct amfh {
	char		*fh_buf;	/* whole file, 0 for the directory */
	unsigned long	fh_len, fh_pos;
	int		fh_dir;
	int		fh_own;		/* fh_buf from am_alloc, not the host */
};

/* ------------------------------------------------------------ names */

static int
am_path(name, path, dirp)
	char *name, *path;
	int *dirp;
{
	char *last = name, *p, *q;
	int n;

	if (am_confdir[0] == 0)
		return -1;
	for (p = name; *p; p++)
		if (*p == ':' || *p == '/')
			last = p + 1;
	*dirp = 0;
	for (p = am_confdir, q = path; *p; )
		*q++ = *p++;
	for (p = q; p > path && p[-1] != '/'; p--)
		;
	if (*last == 0 || am_stricmp(last, p, -1L) == 0) {
		*dirp = 1;
		*q = 0;
		return 0;
	}
	*q++ = '/';
	for (n = 0; last[n]; n++) {
		if (n >= 31 || (last[0] == '.' && (last[1] == 0 ||
		    (last[1] == '.' && last[2] == 0))))
			return -1;
		*q++ = last[n];
	}
	*q = 0;
	return 0;
}

static void
am_setioerr(e)
	long e;
{
	char *t = AP(am_sysbase, EB_THISTASK);

	AL(t, PR_RESULT2) = e;
}

/* open by AmigaOS name: a handle on the whole file, or on the directory */
static struct amfh *
am_fopen(name, dirok)
	char *name;
	int dirok;
{
	char path[100];
	struct amfh *f;
	char *buf = 0;
	unsigned long len = 0;
	int dir, e;

	if (name == 0 || am_path(name, path, &dir) != 0) {
		am_setioerr((long)ERROR_OBJECT_NOT_FOUND);
		return 0;
	}
	if (!dir && (e = amx_readfile(path, &buf, &len, AM_MAXFILE)) != 0) {
		if (!dirok || e != AMX_EISDIR) {
			am_setioerr((long)ERROR_OBJECT_NOT_FOUND);
			return 0;
		}
		dir = 1;
	}
	if (dir && !dirok) {
		if (buf)
			amx_freefile(buf, len);
		am_setioerr((long)ERROR_OBJECT_WRONG_TYPE);
		return 0;
	}
	f = (struct amfh *)am_alloc((unsigned long)sizeof *f,
	    MEMF_PUBLIC | MEMF_CLEAR);
	if (f == 0) {
		if (buf)
			amx_freefile(buf, len);
		am_setioerr((long)ERROR_NO_FREE_STORE);
		return 0;
	}
	f->fh_buf = buf;
	f->fh_len = len;
	f->fh_dir = dir;
	return f;
}

static void
am_fclose(f)
	struct amfh *f;
{
	if (f == 0)
		return;
	if (f->fh_buf && f->fh_own)
		am_free(f->fh_buf, f->fh_len);
	else if (f->fh_buf)
		amx_freefile(f->fh_buf, f->fh_len);
	am_free((char *)f, (unsigned long)sizeof *f);
}

#define	FH(b)	((struct amfh *)((b) << 2))
#define	BP(p)	((unsigned long)(p) >> 2)

/* ------------------------------------------------------------ files */

static void
d_open(r)
	unsigned long *r;
{
	struct amfh *f = 0;

	if (r[D2] == MODE_OLDFILE)
		f = am_fopen((char *)r[D1], 0);
	else
		am_setioerr((long)ERROR_WRITE_PROTECTED);
	r[D0] = BP(f);
}

static void
d_close(r)
	unsigned long *r;
{
	am_fclose(FH(r[D1]));
	r[D0] = 0xffffffff;
}

static void
d_read(r)
	unsigned long *r;
{
	struct amfh *f = FH(r[D1]);
	char *b = (char *)r[D2];
	long n = r[D3], i;

	if (f == 0 || f->fh_dir || n < 0) {
		r[D0] = 0xffffffff;
		return;
	}
	if (n > f->fh_len - f->fh_pos)
		n = f->fh_len - f->fh_pos;
	for (i = 0; i < n; i++)
		b[i] = f->fh_buf[f->fh_pos + i];
	f->fh_pos += n;
	r[D0] = n;
}

static void
d_write(r)
	unsigned long *r;
{
	am_setioerr((long)ERROR_WRITE_PROTECTED);
	r[D0] = 0xffffffff;
}

static void
d_seek(r)
	unsigned long *r;
{
	struct amfh *f = FH(r[D1]);
	long o = r[D2], base, old;

	if (f == 0 || f->fh_dir) {
		r[D0] = 0xffffffff;
		return;
	}
	old = f->fh_pos;
	base = (long)r[D3] == OFFSET_BEGINNING ? 0 :
	    (long)r[D3] == OFFSET_END ? (long)f->fh_len : old;
	if (base + o < 0 || base + o > (long)f->fh_len) {
		am_setioerr((long)ERROR_SEEK_ERROR);
		r[D0] = 0xffffffff;
		return;
	}
	f->fh_pos = base + o;
	r[D0] = old;
}

static void
d_fgetc(r)
	unsigned long *r;
{
	struct amfh *f = FH(r[D1]);

	if (f == 0 || f->fh_dir || f->fh_pos >= f->fh_len)
		r[D0] = 0xffffffff;
	else
		r[D0] = (unsigned char)f->fh_buf[f->fh_pos++];
}

/* FGets: up to len - 1 characters, through the first newline */
static void
d_fgets(r)
	unsigned long *r;
{
	struct amfh *f = FH(r[D1]);
	char *b = (char *)r[D2];
	long n = r[D3], i = 0;
	char c;

	if (f == 0 || f->fh_dir || n <= 0 || f->fh_pos >= f->fh_len) {
		r[D0] = 0;
		return;
	}
	while (i < n - 1 && f->fh_pos < f->fh_len) {
		c = f->fh_buf[f->fh_pos++];
		b[i++] = c;
		if (c == '\n')
			break;
	}
	b[i] = 0;
	r[D0] = (unsigned long)b;
}

static void
d_lock(r)
	unsigned long *r;
{
	r[D0] = BP(am_fopen((char *)r[D1], 1));
}

static void
d_unlock(r)
	unsigned long *r;
{
	am_fclose(FH(r[D1]));
}

static void
d_duplock(r)
	unsigned long *r;
{
	struct amfh *f = FH(r[D1]), *g;
	unsigned long i;

	g = (struct amfh *)am_alloc((unsigned long)sizeof *g,
	    MEMF_PUBLIC | MEMF_CLEAR);
	if (g && f && f->fh_buf) {		/* a file: its own copy */
		g->fh_buf = am_alloc(f->fh_len, MEMF_PUBLIC);
		if (g->fh_buf == 0) {
			am_free((char *)g, (unsigned long)sizeof *g);
			g = 0;
		} else {
			for (i = 0; i < f->fh_len; i++)
				g->fh_buf[i] = f->fh_buf[i];
			g->fh_len = f->fh_len;
			g->fh_own = 1;
		}
	} else if (g)
		g->fh_dir = 1;
	r[D0] = BP(g);
}

/* every directory is the one directory: only the bookkeeping matters */
static void
d_currentdir(r)
	unsigned long *r;
{
	char *t = AP(am_sysbase, EB_THISTASK);

	r[D0] = AL(t, PR_CURRENTDIR);
	AL(t, PR_CURRENTDIR) = r[D1];
}

static void
d_ioerr(r)
	unsigned long *r;
{
	r[D0] = AL(AP(am_sysbase, EB_THISTASK), PR_RESULT2);
}

static void
d_setioerr(r)
	unsigned long *r;
{
	char *t = AP(am_sysbase, EB_THISTASK);

	r[D0] = AL(t, PR_RESULT2);
	AL(t, PR_RESULT2) = r[D1];
}

static void
d_loadseg(r)
	unsigned long *r;
{
	struct amfh *f = am_fopen((char *)r[D1], 0);
	unsigned long seg = 0;
	int e;

	if (f) {
		seg = am_loadseg(f->fh_buf, f->fh_len, &e);
		if (seg == 0)
			am_setioerr(e == AMH_ENOMEM ? (long)ERROR_NO_FREE_STORE :
			    (long)ERROR_OBJECT_WRONG_TYPE);
		am_fclose(f);
	}
	r[D0] = seg;
}

static void
d_unloadseg(r)
	unsigned long *r;
{
	am_unloadseg(r[D1]);
	r[D0] = 0xffffffff;
}

static void
d_delay(r)
	unsigned long *r;
{
	unsigned long t = r[D1];

	while (t--)
		amx_delayus(20000L);		/* 50 ticks a second */
}

static void
d_allocdosobject(r)
	unsigned long *r;
{
	unsigned long n = r[D1] == DOS_RDARGS ? RDA_SIZE :
	    r[D1] == DOS_FIB ? FIB_SIZE : 0;

	r[D0] = n ? (unsigned long)am_alloc(n, MEMF_PUBLIC | MEMF_CLEAR) : 0;
}

static void
d_freedosobject(r)
	unsigned long *r;
{
	char *p = (char *)r[D2];

	if (p && r[D1] == DOS_RDARGS)
		am_free(p, (unsigned long)RDA_SIZE);
	else if (p && r[D1] == DOS_FIB)
		am_free(p, (unsigned long)FIB_SIZE);
}

/* ---------------------------------------------------------- numbers */

/* StrToLong: optional sign, decimal digits; characters used, or -1 */
static long
am_strtol(s, vp)
	char *s;
	long *vp;
{
	char *p = s;
	long v = 0;
	int neg = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-' || *p == '+')
		neg = *p++ == '-';
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + *p++ - '0';
	*vp = neg ? -v : v;
	return p - s;
}

static void
d_strtolong(r)
	unsigned long *r;
{
	long v;

	r[D0] = am_strtol((char *)r[D1], &v);
	if ((long)r[D0] >= 0)
		*(long *)r[D2] = v;
}

/* --------------------------------------------------------- ReadArgs */

struct item {
	char	*names;			/* in the template, up to ',' or '/' */
	int	len;
	int	f;			/* the flags below */
};
#define	IF_A	0x01
#define	IF_K	0x02
#define	IF_S	0x04
#define	IF_N	0x08
#define	IF_T	0x10
#define	IF_M	0x20
#define	IF_F	0x40

static int
am_template(t, it)
	char *t;
	struct item *it;
{
	int n = 0;

	while (*t) {
		if (n == AM_MAXITEMS)
			return -1;
		it[n].names = t;
		while (*t && *t != ',' && *t != '/')
			t++;
		it[n].len = t - it[n].names;
		it[n].f = 0;
		while (*t == '/') {
			switch (t[1]) {
			case 'A': case 'a': it[n].f |= IF_A; break;
			case 'K': case 'k': it[n].f |= IF_K; break;
			case 'S': case 's': it[n].f |= IF_S; break;
			case 'N': case 'n': it[n].f |= IF_N; break;
			case 'T': case 't': it[n].f |= IF_T; break;
			case 'M': case 'm': it[n].f |= IF_M; break;
			case 'F': case 'f': it[n].f |= IF_F; break;
			default: return -1;
			}
			t += 2;
		}
		if (*t == ',')
			t++;
		else if (*t)
			return -1;
		n++;
	}
	return n;
}

/* the item one of whose names (A=B=C) is w[0..wl), or -1 */
static int
am_finditem(it, n, w, wl)
	struct item *it;
	int n, wl;
	char *w;
{
	char *p, *e;
	int i;

	for (i = 0; i < n; i++)
		for (p = it[i].names, e = p + it[i].len; p < e; ) {
			char *q = p;

			while (q < e && *q != '=')
				q++;
			if (q - p == wl && am_stricmp(p, w, (long)wl) == 0)
				return i;
			p = q < e ? q + 1 : q;
		}
	return -1;
}

static void
d_findarg(r)
	unsigned long *r;
{
	struct item it[AM_MAXITEMS];
	char *k = (char *)r[D2];
	int n = am_template((char *)r[D1], it), l = 0;

	while (k[l])
		l++;
	r[D0] = n < 0 ? 0xffffffff : (unsigned long)(long)am_finditem(it, n, k, l);
}

/* the arena: [size][flags] then strings, longs and /M arrays */
#define	AR_HDR		8
#define	AR_OWNRDA	1

struct rd {
	char		*in, *end;	/* the input, up to '\n' */
	char		*ar, *ap, *ae;	/* the arena */
	long		err;
};

static char *
rd_take(d, n)
	struct rd *d;
	long n;
{
	char *p;

	d->ap = (char *)(((unsigned long)d->ap + 3) & ~3);
	if (d->ap + n > d->ae)
		return 0;
	p = d->ap;
	d->ap += n;
	return p;
}

/* the next word: 0 at the end of the line; *eqp set when it stopped at
 * '=', *qp when quoted */
static char *
rd_word(d, eqp, qp)
	struct rd *d;
	int *eqp, *qp;
{
	char *o, *s;
	char c;

	*eqp = *qp = 0;
	while (d->in < d->end && (*d->in == ' ' || *d->in == '\t'))
		d->in++;
	if (d->in >= d->end)
		return 0;
	o = s = rd_take(d, (long)(d->end - d->in) + 1);
	if (o == 0) {
		d->err = ERROR_NO_FREE_STORE;
		return 0;
	}
	if (*d->in == '"') {
		*qp = 1;
		for (d->in++; ; d->in++) {
			if (d->in >= d->end) {
				d->err = ERROR_UNMATCHED_QUOTES;
				return 0;
			}
			c = *d->in;
			if (c == '"') {
				d->in++;
				break;
			}
			if (c == '*' && d->in + 1 < d->end) {
				c = *++d->in;
				c = (c == 'n' || c == 'N') ? '\n' :
				    (c == 'e' || c == 'E') ? 0x1b : c;
			}
			*s++ = c;
		}
	} else
		while (d->in < d->end && *d->in != ' ' && *d->in != '\t') {
			if (*d->in == '=') {
				*eqp = 1;
				d->in++;
				break;
			}
			*s++ = *d->in++;
		}
	*s = 0;
	d->ap = s + 1;			/* keep only what the word used */
	return o;
}

/* the rest of the line, unparsed, without trailing blanks: /F */
static char *
rd_rest(d)
	struct rd *d;
{
	char *o, *s, *e = d->end;

	while (d->in < e && (*d->in == ' ' || *d->in == '\t'))
		d->in++;
	while (e > d->in && (e[-1] == ' ' || e[-1] == '\t'))
		e--;
	if ((o = rd_take(d, (long)(e - d->in) + 1)) == 0) {
		d->err = ERROR_NO_FREE_STORE;
		return 0;
	}
	for (s = o; d->in < e; )
		*s++ = *d->in++;
	*s = 0;
	d->in = d->end;
	return o;
}

/* a value for item i into the array (/M words collect in ml) */
static int
rd_set(d, it, i, w, a, ml, mn)
	struct rd *d;
	struct item *it;
	int i;
	char *w, **ml;
	unsigned long *a;
	int *mn;
{
	long v, *lp, k;

	if (it[i].f & IF_M) {
		ml[(*mn)++] = w;
		return 0;
	}
	if (it[i].f & IF_N) {
		k = am_strtol(w, &v);
		if (k < 0 || w[k] != 0) {
			d->err = ERROR_BAD_NUMBER;
			return -1;
		}
		if ((lp = (long *)rd_take(d, 4L)) == 0) {
			d->err = ERROR_NO_FREE_STORE;
			return -1;
		}
		*lp = v;
		a[i] = (unsigned long)lp;
		return 0;
	}
	if (it[i].f & IF_T) {
		a[i] = (am_stricmp(w, "yes", -1L) == 0 ||
		    am_stricmp(w, "on", -1L) == 0) ? 0xffffffff : 0;
		return 0;
	}
	a[i] = (unsigned long)w;
	return 0;
}

static void
d_readargs(r)
	unsigned long *r;
{
	struct item it[AM_MAXITEMS];
	char set[AM_MAXITEMS];
	unsigned long *a = (unsigned long *)r[D2];
	char *rda = (char *)r[D3], *w, *ws, **ml, **mv;
	struct rd d;
	int n, i, k, eq, q, wl, mi = -1, mn = 0, own = 0;
	long sz;

	n = am_template((char *)r[D1], it);
	if (n < 0) {
		am_setioerr((long)ERROR_BAD_TEMPLATE);
		r[D0] = 0;
		return;
	}
	if (rda == 0) {
		rda = am_alloc((unsigned long)RDA_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
		own = 1;
		if (rda == 0) {
			am_setioerr((long)ERROR_NO_FREE_STORE);
			r[D0] = 0;
			return;
		}
	}
	/* the input: the CSource, through its first newline */
	d.in = d.end = 0;
	if (AP(rda, CS_BUFFER)) {
		d.in = AP(rda, CS_BUFFER) + AL(rda, CS_CURCHR);
		d.end = AP(rda, CS_BUFFER) + AL(rda, CS_LENGTH);
		for (w = d.in; w < d.end && *w != '\n' && *w; w++)
			;
		d.end = w;
		AL(rda, CS_CURCHR) = (w < AP(rda, CS_BUFFER) + AL(rda, CS_LENGTH) &&
		    *w == '\n' ? w + 1 : w) - AP(rda, CS_BUFFER);
	}
	/* words (each up to 3 bytes of alignment), the /M lists, numbers */
	sz = AR_HDR + 8 * (d.end - d.in) + 16 * n + 64;
	d.ar = am_alloc((unsigned long)sz, MEMF_PUBLIC | MEMF_CLEAR);
	if (d.ar == 0) {
		if (own)
			am_free(rda, (unsigned long)RDA_SIZE);
		am_setioerr((long)ERROR_NO_FREE_STORE);
		r[D0] = 0;
		return;
	}
	AL(d.ar, 0) = sz;
	AL(d.ar, 4) = own ? AR_OWNRDA : 0;
	d.ap = d.ar + AR_HDR;
	d.ae = d.ar + sz;
	d.err = 0;
	for (i = 0; i < n; i++) {
		set[i] = 0;
		if (it[i].f & IF_M)
			mi = i;
	}
	ml = (char **)rd_take(&d, 4L * ((d.end - d.in) / 2 + 2));
	if (ml == 0)
		d.err = ERROR_NO_FREE_STORE;

	for (;;) {
		ws = d.in;
		if (d.err || (w = rd_word(&d, &eq, &q)) == 0)
			break;
		for (wl = 0; w[wl]; wl++)
			;
		k = q ? -1 : am_finditem(it, n, w, wl);
		if (k >= 0) {
			if (it[k].f & IF_S) {
				a[k] = 0xffffffff;
				set[k] = 1;
				continue;
			}
			if (it[k].f & IF_F)
				w = rd_rest(&d);
			else if ((w = rd_word(&d, &eq, &q)) == 0) {
				if (!d.err)
					d.err = ERROR_KEY_NEEDS_ARG;
				break;
			}
			if (w == 0 || rd_set(&d, it, k, w, a, ml, &mn))
				break;
			set[k] = 1;
			continue;
		}
		/* positional: the first open item that takes one */
		for (k = 0; k < n; k++)
			if (!(it[k].f & (IF_K | IF_S | IF_T)) &&
			    (!set[k] || (it[k].f & IF_M)))
				break;
		if (k == n) {
			d.err = ERROR_TOO_MANY_ARGS;
			break;
		}
		if (it[k].f & IF_F) {		/* the line from this word on */
			d.in = ws;
			if ((w = rd_rest(&d)) == 0)
				break;
		}
		if (rd_set(&d, it, k, w, a, ml, &mn))
			break;
		set[k] = 1;
	}
	/* /M gives its last words to required items after it */
	if (!d.err && mi >= 0)
		for (i = n - 1; i > mi; i--)
			if ((it[i].f & IF_A) && !set[i] &&
			    !(it[i].f & (IF_K | IF_S)) && mn > 0) {
				if (rd_set(&d, it, i, ml[--mn], a, ml, &mn))
					break;
				set[i] = 1;
			}
	if (!d.err && mi >= 0 && mn > 0) {
		mv = (char **)rd_take(&d, 4L * (mn + 1));
		if (mv == 0)
			d.err = ERROR_NO_FREE_STORE;
		else {
			for (i = 0; i < mn; i++)
				mv[i] = ml[i];
			mv[mn] = 0;
			a[mi] = (unsigned long)mv;
		}
	}
	for (i = 0; !d.err && i < n; i++)
		if ((it[i].f & IF_A) && !set[i])
			d.err = ERROR_REQUIRED_ARG_MISSING;
	if (d.err) {
		am_free(d.ar, (unsigned long)sz);
		if (own)
			am_free(rda, (unsigned long)RDA_SIZE);
		am_setioerr(d.err);
		r[D0] = 0;
		return;
	}
	AP(rda, RDA_DALIST) = d.ar;
	r[D0] = (unsigned long)rda;
}

static void
d_freeargs(r)
	unsigned long *r;
{
	char *rda = (char *)r[D1], *ar;
	int own;

	if (rda == 0 || (ar = AP(rda, RDA_DALIST)) == 0)
		return;
	own = AL(ar, 4) & AR_OWNRDA;
	AL(rda, RDA_DALIST) = 0;
	am_free(ar, AL(ar, 0));
	if (own)
		am_free(rda, (unsigned long)RDA_SIZE);
}

static struct amfn am_dostab[] = {
	{ -30, d_open },
	{ -36, d_close },
	{ -42, d_read },
	{ -48, d_write },
	{ -66, d_seek },
	{ -84, d_lock },
	{ -90, d_unlock },
	{ -96, d_duplock },
	{ -126, d_currentdir },
	{ -132, d_ioerr },
	{ -150, d_loadseg },
	{ -156, d_unloadseg },
	{ -198, d_delay },
	{ -228, d_allocdosobject },
	{ -234, d_freedosobject },
	{ -306, d_fgetc },
	{ -336, d_fgets },
	{ -462, d_setioerr },
	{ -798, d_readargs },
	{ -804, d_findarg },
	{ -816, d_strtolong },
	{ -858, d_freeargs },
	{ 0, 0 }
};

struct amlib am_dos = {
	"dos.library", NT_LIBRARY, 40, 160, 64, am_dostab
};

/* ------------------------------------------------- intuition: alerts */

/* DisplayAlert(code d0, string a0, height d1): lines of [x.w y.b text 0
 * more.b]; each goes to the console */
static void
i_displayalert(r)
	unsigned long *r;
{
	unsigned char *s = (unsigned char *)r[A0];
	int more = 1;

	while (s && more) {
		s += 3;
		amx_log("alert: %s\n", (long)s, 0L, 0L, 0L);
		while (*s)
			s++;
		more = s[1];
		s += 2;
	}
	r[D0] = 0;
}

static struct amfn am_inttab[] = {
	{ -90, i_displayalert },
	{ 0, 0 }
};

struct amlib am_intuition = {
	"intuition.library", NT_LIBRARY, 40, 20, 64, am_inttab
};
