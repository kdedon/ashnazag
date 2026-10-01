/*
 * hostfs.c -- drives served from Unix directories (C:, U:, others the
 * launcher names): GEMDOS file calls made with host system calls, so
 * the user's own permissions apply.  Other drives go to TOS.
 *
 * Unix names that fit 8.3 in one case show uppercased; others get a
 * short name from a hash ("Makefile" -> "MAK~3F2A"), found again by
 * scanning the directory.  Below a root other than "/", ".." and
 * symbolic links resolve as if the root were "/": a guard against
 * accidents, not a security boundary, since TOS programs run as the
 * user and can make host calls themselves.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>
#include <fcntl.h>
#include <utime.h>

#define	NDRV	19			/* drives served: C: to U: */
#define	HBASE	0x400			/* our handles: HBASE + descriptor */
#define	NH	32
#define	NS	8			/* searches in progress */
#define	PLEN	256

#define	E_FILNF	-33
#define	E_PTHNF	-34
#define	E_NHNDL	-35
#define	E_ACCDN	-36
#define	E_IHNDL	-37
#define	E_NSMEM	-39
#define	E_NSAME	-48
#define	E_NMFIL	-49

#define	FA_RDONLY 0x01
#define	FA_HIDDEN 0x02
#define	FA_LABEL 0x08
#define	FA_DIR	0x10

/* host calls: -errno on failure */
extern long sys_read(), sys_write(), sys_open(), sys_close(), sys_unlink(), sys_chmod();
extern long sys_lseek(), sys_utime(), sys_access(), sys_rmdir(), sys_mkdir();
extern long sys_getdents(), sys_readlink(), sys_statvfs();
extern long sys_rename();
/* GEMDOS, through the vector */
extern long Pexec(), Mfree();

extern char p_dtab[];			/* set by the launcher: letter, flags, path; ... 0 */
extern long p_tz;			/* seconds east of UTC */

struct drive {
	char	*root;
	int	ro;
	char	cwd[PLEN];		/* below the root, '/'-separated */
};

struct handle {
	int	fd;			/* 0: free, else descriptor + 1 */
	int	d;
	char	rel[PLEN];		/* for Fdatime */
};

struct search {
	char	*dta;			/* 0: free */
	int	fd, n, d;
	long	age;
	char	rel[PLEN];
	char	pat[14];
	int	attr, root;
	char	buf[1024];
	int	pos, len;
};

static struct handle hd[NH];
static struct search sr[NS];
static long age;
static struct drive dv[NDRV];
static char dmap[32];			/* drive number -> dv index + 1 */
static struct drive *cur;		/* the drive of the call being served */
static char *root;			/* its root */
static char hp[2 * PLEN];		/* host path of the last resolve */
static char rel[PLEN];
static char pend[2 * PLEN];
static char lk[2 * PLEN];
static struct stat st;

static char ok83[] = "!#$%&'()-@^_`{}~";

static int
up(c)
	int c;
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int
low(c)
	int c;
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
slen(s)
	char *s;
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void
scpy(d, s)
	char *d, *s;
{
	while ((*d++ = *s++) != 0)
		;
}

static int
okc(c)
	int c;
{
	char *p;

	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return 1;
	for (p = ok83; *p; p++)
		if (*p == c)
			return 1;
	return 0;
}

static long
L(p)
	short *p;
{
	return (long)p[0] << 16 | (unsigned short)p[1];
}

static long
err(e)
	long e;
{
	switch (-e) {
	case 2: return E_FILNF;
	case 20: return E_PTHNF;
	case 9: return E_IHNDL;
	case 23: case 24: return E_NHNDL;
	case 12: return E_NSMEM;
	case 18: return E_NSAME;
	case 22: return -32;
	}
	return E_ACCDN;
}

/* the running program's basepage, through the OS header */
static char *
run()
{
	char *os = *(char **)0x4f2;

	os = *(char **)(os + 8);
	return **(char ***)(os + 0x28);
}

/* the default drive; -1 while the basepage is not a real one (early boot) */
static int
Dgetdrv()
{
	char *bp = run();

	return bp && bp < *(char **)0x42e ? bp[0x37] : -1;
}
#define	Fgetdta()	(*(char **)(run() + 0x20))

/* a host call's result: 0, or a GEMDOS error */
static long
sc(r)
	long r;
{
	return r < 0 ? err(r) : 0;
}

/* the 8.3 name TOS sees for host name n */
static void
short83(n, o)
	char *n, *o;
{
	char *d = 0, *p;
	int i, b = 0, e = 0, lo = 0, hi = 0, bad = n[0] == '.';
	unsigned h = 0;

	if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) {
		scpy(o, n);
		return;
	}
	for (p = n; *p; p++) {
		h = h * 31 + (*p & 0xff);
		if (*p == '.') {
			bad |= d != 0;
			d = p;
		} else if (!okc(*p))
			bad = 1;
		else if (d)
			e++;
		else
			b++;
		lo |= *p >= 'a' && *p <= 'z';
		hi |= *p >= 'A' && *p <= 'Z';
	}
	if (!bad && !(lo && hi) && b >= 1 && b <= 8 && e <= 3 && !(d && e == 0)) {
		for (p = n; *p; p++)
			*o++ = up(*p);
		*o = 0;
		return;
	}
	d = 0;
	for (p = n + 1; *p; p++)
		if (*p == '.')
			d = p;
	for (i = 0, p = n; *p && p != d && i < 3; p++)
		if (okc(*p))
			o[i++] = up(*p);
	if (i == 0)
		o[i++] = '_';
	o[i++] = '~';
	for (b = 12; b >= 0; b -= 4)
		o[i++] = "0123456789ABCDEF"[(h >> b) & 15];
	if (d) {
		o[i++] = '.';
		for (e = 0, p = d + 1; *p && e < 3; p++)
			if (okc(*p)) {
				o[i++] = up(*p);
				e++;
			}
		if (e == 0)
			i--;
	}
	o[i] = 0;
}

static int
sameci(a, b)
	char *a, *b;
{
	while (*a && up(*a) == up(*b))
		a++, b++;
	return *a == 0 && *b == 0;
}

/* host path of rel (below the root) into hp, with name appended if given */
static char *
host(r, name)
	char *r, *name;
{
	char *d = hp;

	/* too long: an empty path, which the host call then refuses */
	if (slen(root) + slen(r) + (name ? slen(name) : 0) + 3 > sizeof hp) {
		hp[0] = 0;
		return hp;
	}
	scpy(d, root);
	d += slen(d);
	if (d[-1] != '/')
		*d++ = '/';
	scpy(d, r);
	d += slen(d);
	if (name) {
		if (r[0])
			*d++ = '/';
		scpy(d, name);
	}
	return hp;
}

/* the host name in directory r whose short name is t; 1 if found */
static int
lookup(r, t, out)
	char *r, *t, *out;
{
	static char b[512];
	char s[14];
	struct dirent *de;
	long n, i;
	int fd, k;

	for (k = 0; k < 2; k++) {
		for (i = 0; t[i] && i < 13; i++)
			out[i] = k ? t[i] : low(t[i]);
		out[i] = 0;
		short83(out, s);
		if (sameci(s, t) && _lxstat(2, host(r, out), &st) == 0)
			return 1;
	}
	if ((fd = sys_open(host(r, (char *)0), O_RDONLY, 0)) < 0)
		return 0;
	while ((n = sys_getdents(fd, b, (long)sizeof b)) > 0)
		for (i = 0; i < n; i += de->d_reclen) {
			de = (struct dirent *)(b + i);
			short83(de->d_name, s);
			if (sameci(s, t) && slen(de->d_name) < 64) {
				scpy(out, de->d_name);
				sys_close(fd);
				return 1;
			}
		}
	sys_close(fd);
	for (i = 0; t[i] && i < 13; i++)
		out[i] = low(t[i]);
	out[i] = 0;
	return 0;
}

static void
pop(r)
	char *r;
{
	char *p = r + slen(r);

	while (p > r && *--p != '/')
		;
	*p = 0;
}

static int
push(r, n)
	char *r, *n;
{
	int l = slen(r);

	if (l + slen(n) + 2 > PLEN)
		return -1;
	if (l)
		r[l++] = '/';
	scpy(r + l, n);
	return 0;
}

/*
 * Append host name n to rel; below a root other than "/", follow
 * symbolic links with the root as "/".  last: n ends the path, nf: do
 * not follow it there.
 */
static int
walk(n, last, nf)
	char *n;
	int last, nf;
{
	static int links;
	char *c, *e;
	long k;
	int l;

	if (slen(n) >= PLEN)
		return -1;
	scpy(pend, n);
	links = 0;
	while (pend[0]) {
		for (e = pend; *e && *e != '/'; e++)
			;
		c = pend;
		l = e - c;
		if (*e)
			*e++ = 0;
		if (l == 0 || (l == 1 && c[0] == '.'))
			;
		else if (l == 2 && c[0] == '.' && c[1] == '.')
			pop(rel);
		else {
			if (push(rel, c) < 0)
				return -1;
			if (!(root[0] == '/' && root[1] == 0) && !(last && nf && *e == 0) &&
			    (k = sys_readlink(host(rel, (char *)0), lk, (long)PLEN - 1)) > 0) {
				if (++links > 8 || k + slen(e) + 2 > sizeof pend)
					return -1;
				lk[k] = 0;
				pop(rel);
				if (lk[0] == '/')
					rel[0] = 0;
				lk[k++] = '/';
				scpy(lk + k, e);
				scpy(pend, lk);
				continue;
			}
		}
		scpy(pend, e);
	}
	return 0;
}

/* select drive d if served here (cur, root); 1 if so */
static int
sel(d)
	int d;
{
	if (d < 0 || d >= 32 || !dmap[d])
		return 0;
	cur = dv + dmap[d] - 1;
	root = cur->root;
	return 1;
}

/* drive of a TOS path, the path after it */
static int
drive(p, rest)
	char *p, **rest;
{
	if (p[0] && p[1] == ':') {
		*rest = p + 2;
		return up(p[0]) - 'A';
	}
	*rest = p;
	return Dgetdrv();
}

/*
 * The TOS path t (drive removed) into rel and hp.  0: it exists, 1:
 * only its last component is missing (its name lowercased), else an error.
 */
static int
resolve(t, nf)
	char *t;
	int nf;
{
	char c[14], n[64];
	int i, found, last;

	if (*t == '\\' || *t == '/')
		rel[0] = 0;
	else
		scpy(rel, cur->cwd);
	for (;;) {
		while (*t == '\\' || *t == '/')
			t++;
		if (*t == 0)
			break;
		for (i = 0; *t && *t != '\\' && *t != '/'; t++)
			if (i < 13)
				c[i++] = *t;
		c[i] = 0;
		while (*t == '\\' || *t == '/')
			t++;
		last = *t == 0;
		if ((c[0] == '.' && c[1] == 0) || (c[0] == '.' && c[1] == '.' && c[2] == 0)) {
			if (c[1])
				pop(rel);
			continue;
		}
		found = lookup(rel, c, n);
		if (!found && !last)
			return E_PTHNF;
		if (walk(n, last, nf) < 0)
			return E_PTHNF;
		if (!found) {
			host(rel, (char *)0);
			return 1;
		}
	}
	host(rel, (char *)0);
	return 0;
}

static int
isdir()
{
	return (st.st_mode & S_IFMT) == S_IFDIR;
}

/* DOS time and date words of a Unix time */
static void
dostime(t, w)
	long t;
	unsigned short *w;
{
	static char md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	long d, s;
	int y = 1970, m = 0, n;

	t += p_tz;
	if (t < 315532800L)
		t = 315532800L;		/* 1980 */
	d = t / 86400;
	s = t % 86400;
	for (;; y++) {
		n = y % 4 == 0 ? 366 : 365;
		if (d < n)
			break;
		d -= n;
	}
	for (;; m++) {
		n = md[m] + (m == 1 && y % 4 == 0);
		if (d < n)
			break;
		d -= n;
	}
	w[0] = (s / 3600) << 11 | (s / 60 % 60) << 5 | (s % 60) / 2;
	w[1] = (y - 1980) << 9 | (m + 1) << 5 | (d + 1);
}

static long
unixtime(w)
	unsigned short *w;
{
	static short cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	int y = (w[1] >> 9) + 1980, m = (w[1] >> 5 & 15) - 1, d = (w[1] & 31) - 1;
	long days;

	if (m < 0 || m > 11)
		m = 0;
	days = (y - 1970) * 365L + (y - 1969) / 4 + cum[m] + d + (m > 1 && y % 4 == 0);
	return days * 86400 + (w[0] >> 11) * 3600L + (w[0] >> 5 & 63) * 60 + (w[0] & 31) * 2 -
	    p_tz;
}

/* attributes of the file at hp, after a stat into st */
static int
attrs(name)
	char *name;
{
	int a = 0;

	if (isdir())
		a |= FA_DIR;
	else if (cur->ro || sys_access(hp, 2L) < 0)
		a |= FA_RDONLY;
	if (name[0] == '.' && name[1] && !(name[1] == '.' && name[2] == 0))
		a |= FA_HIDDEN;
	return a;
}

/* "*" and "?" against an 8.3 name, name and extension apart */
static int
part(p, pe, n, ne)
	char *p, *pe, *n, *ne;
{
	for (; p < pe; p++, n++) {
		if (*p == '*')
			return 1;
		if (n >= ne ? *p != '?' : *p != '?' && up(*p) != up(*n))
			return 0;
	}
	return n >= ne;
}

static int
match(p, n)
	char *p, *n;
{
	char *pd = p + slen(p), *nd = n + slen(n), *q;

	if (n[0] == '.')
		return sameci(p, n) || (p[0] == '*' && p[1] == '.' && p[2] == '*' && p[3] == 0) ||
		    (p[0] == '*' && p[1] == 0);
	for (q = p; *q; q++)
		if (*q == '.')
			pd = q;
	for (q = n; *q; q++)
		if (*q == '.')
			nd = q;
	if (!part(p, pd, n, nd))
		return 0;
	if (*pd == 0)
		return *nd == 0 || pd[-1] == '*';
	return part(pd + 1, pd + slen(pd), *nd ? nd + 1 : nd, nd + slen(nd));
}

static long
put32(p, v)
	char *p;
	long v;
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
	return v;
}

/* the next match of search s into its DTA */
static long
next(s)
	struct search *s;
{
	struct dirent *de;
	char sn[14], *d = s->dta;
	unsigned short w[2];
	int a;
	long sz;

	cur = dv + s->d;
	root = cur->root;
	for (;;) {
		if (s->pos >= s->len) {
			s->len = sys_getdents(s->fd, s->buf, (long)sizeof s->buf);
			s->pos = 0;
			if (s->len <= 0) {
				sys_close(s->fd);
				s->dta = 0;
				return s->n ? E_NMFIL : E_FILNF;
			}
		}
		de = (struct dirent *)(s->buf + s->pos);
		s->pos += de->d_reclen;
		if (s->root && de->d_name[0] == '.' &&
		    (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
			continue;
		if (slen(de->d_name) >= 64)
			continue;
		short83(de->d_name, sn);
		if (!match(s->pat, sn))
			continue;
		host(s->rel, de->d_name);
		if (_xstat(2, hp, &st) < 0 && _lxstat(2, hp, &st) < 0)
			continue;
		a = attrs(de->d_name);
		if (a & (FA_HIDDEN | FA_DIR) & ~s->attr)
			continue;
		break;
	}
	s->n++;
	d[21] = a;
	dostime(st.st_mtime, w);
	d[22] = w[0] >> 8; d[23] = w[0];
	d[24] = w[1] >> 8; d[25] = w[1];
	sz = isdir() ? 0 : st.st_size;
	put32(d + 26, sz);
	scpy(d + 30, sn);
	return 0;
}


static char dp[2 * PLEN];

static long
fsfirst(t, attr)
	char *t;
	int attr;
{
	struct search *s, *o = 0;
	char *dta = Fgetdta(), *b = t, *p;
	int i;

	for (p = t; *p; p++)
		if (*p == '\\' || *p == '/')
			b = p + 1;
	if (attr == FA_LABEL || slen(b) > 13)
		return E_FILNF;
	for (i = 0; t + i < b && i < PLEN - 1; i++)
		dp[i] = t[i];
	dp[i] = 0;
	/* this DTA's search, else a free one, else the oldest */
	for (s = sr; s < sr + NS; s++)
		if (s->dta == dta)
			o = s;
	for (s = sr; s < sr + NS && !o; s++)
		if (!s->dta)
			o = s;
	if (!o)
		for (o = sr, s = sr + 1; s < sr + NS; s++)
			if (s->age < o->age)
				o = s;
	s = o;
	if (s->dta)
		sys_close(s->fd);
	s->dta = 0;
	if (resolve(dp, 0) != 0 || _xstat(2, hp, &st) < 0 || !isdir())
		return E_PTHNF;
	if ((s->fd = sys_open(hp, O_RDONLY, 0)) < 0)
		return err((long)s->fd);
	scpy(s->rel, rel);
	s->d = cur - dv;
	scpy(s->pat, b[0] ? b : "*.*");
	s->attr = attr;
	s->root = rel[0] == 0;
	s->n = s->pos = s->len = 0;
	s->age = ++age;
	s->dta = dta;
	dta[0] = 'U'; dta[1] = 'H'; dta[2] = 'F'; dta[3] = 'S';
	dta[4] = s - sr;
	return next(s);
}

/* 1 if the DTA holds one of our searches */
static int
fsnext(r)
	long *r;
{
	struct search *s;
	char *dta;

	for (s = sr; s < sr + NS && !s->dta; s++)
		;
	if (s == sr + NS)
		return 0;
	dta = Fgetdta();
	if (dta[0] != 'U' || dta[1] != 'H' || dta[2] != 'F' || dta[3] != 'S' ||
	    (dta[4] & 0xff) >= NS)
		return 0;
	s = sr + dta[4];
	*r = s->dta == dta ? next(s) : E_NMFIL;
	return 1;
}

static long
fopen(t, mode, create, attr)
	char *t;
	int mode, create, attr;
{
	struct handle *h;
	long r, fd;

	if (create && (attr & (FA_LABEL | FA_DIR)))
		return E_ACCDN;
	if ((r = resolve(t, 0)) < 0)
		return r;
	/* regular files only: a FIFO or device would block or reach a terminal */
	if (!create && (r || _xstat(2, hp, &st) < 0 || (st.st_mode & S_IFMT) != S_IFREG))
		return E_FILNF;
	if (create && r == 0 && _xstat(2, hp, &st) == 0 && (st.st_mode & S_IFMT) != S_IFREG)
		return E_ACCDN;
	for (h = hd; h < hd + NH && h->fd; h++)
		;
	if (h == hd + NH)
		return E_NHNDL;
	if (create)
		fd = sys_open(hp, O_RDWR | O_CREAT | O_TRUNC, attr & FA_RDONLY ? 0444 : 0666);
	else
		fd = sys_open(hp, mode & 3, 0);
	if (fd < 0)
		return err(fd);
	h->fd = fd + 1;
	h->d = cur - dv;
	scpy(h->rel, rel);
	return HBASE + (h - hd);
}

static long
datime(h, w, set)
	struct handle *h;
	unsigned short *w;
	int set;
{
	struct utimbuf u;
	long r;

	if (!set) {
		if ((r = _fxstat(2, h->fd - 1, &st)) < 0)
			return err(r);
		dostime(st.st_mtime, w);
		return 0;
	}
	if (dv[h->d].ro)
		return E_ACCDN;
	u.actime = u.modtime = unixtime(w);
	root = dv[h->d].root;
	r = sys_utime(host(h->rel, (char *)0), &u);
	return r < 0 ? err(r) : 0;
}

static long
fattrib(t, set, a)
	char *t;
	int set, a;
{
	char *n;
	long r;

	if (resolve(t, 0) != 0 || _xstat(2, hp, &st) < 0)
		return E_FILNF;
	for (n = rel + slen(rel); n > rel && n[-1] != '/'; n--)
		;
	if (set && !isdir()) {
		r = sys_chmod(hp, a & FA_RDONLY ? st.st_mode & 07555 : (st.st_mode & 07777) | 0200);
		if (r < 0)
			return err(r);
		_xstat(2, hp, &st);
	}
	return attrs(n);
}

static long
dgetpath(b)
	char *b;
{
	char *c = cur->cwd, *e, *b0 = b;
	int n;

	*b = 0;
	while (*c) {
		if (b - b0 + 14 > 128) {
			*b0 = 0;
			return -64;
		}
		for (e = c; *e && *e != '/'; e++)
			;
		n = e - c;
		scpy(lk, c);
		lk[n] = 0;
		*b++ = '\\';
		short83(lk, b);
		b += slen(b);
		c = *e ? e + 1 : e;
	}
	return 0;
}

static long
dfree(b)
	char *b;
{
	struct statvfs v;
	unsigned long cl, fr, tot;
	long r;

	if ((r = sys_statvfs(root, &v)) < 0)
		return err(r);
	cl = v.f_frsize >= 512 ? v.f_frsize / 512 : 1;
	fr = v.f_bavail;
	tot = v.f_blocks;
	/* free bytes stay below 2 GB for 32-bit arithmetic */
	if (fr > 0x7fff0000 / (cl * 512))
		fr = 0x7fff0000 / (cl * 512);
	if (tot > 0x7fff0000 / (cl * 512))
		tot = 0x7fff0000 / (cl * 512);
	put32(b, fr);
	put32(b + 4, tot);
	put32(b + 8, 512L);
	put32(b + 12, cl);
	return 0;
}

static long
get32(p)
	unsigned char *p;
{
	return (long)p[0] << 24 | (long)p[1] << 16 | p[2] << 8 | p[3];
}

/* load a program into a new basepage; the basepage or an error */
static long
pexec(t, cmd, env)
	char *t, *cmd, *env;
{
	static unsigned char h[28], rb[256];
	char *bp, *p, *a;
	long tl, dl, bl, n, i, fd, r;

	if ((r = resolve(t, 0)) != 0)
		return r < 0 ? r : E_FILNF;
	if ((fd = sys_open(hp, O_RDONLY, 0)) < 0)
		return err(fd);
	if (sys_read(fd, h, 28L) != 28 || h[0] != 0x60 || h[1] != 0x1a) {
		sys_close(fd);
		return -66;
	}
	tl = get32(h + 2);
	dl = get32(h + 6);
	bl = get32(h + 10);
	bp = (char *)Pexec(7L, get32(h + 22), cmd, env);
	if ((long)bp < 0) {
		sys_close(fd);
		return (long)bp;
	}
	p = bp + 256;
	r = E_NSMEM;
	n = get32(bp + 4) - get32(bp) - 256;
	if (tl < 0 || dl < 0 || bl < 0 || tl > n || dl > n - tl || bl > n - tl - dl)
		goto fail;
	r = -66;
	if (sys_read(fd, p, tl + dl) != tl + dl)
		goto fail;
	if (h[26] == 0 && h[27] == 0 &&
	    sys_lseek(fd, 28 + tl + dl + get32(h + 14), 0) >= 0 &&
	    sys_read(fd, rb, 4L) == 4 && (i = get32(rb)) != 0) {
		if (i < 0 || i > tl + dl - 4)
			goto fail;
		a = p + i;
		n = i = 0;
		for (;;) {
			if (a < p || a > p + tl + dl - 4)
				goto fail;
			*(long *)a += (long)p;
			do {
				if (i == n && ((n = sys_read(fd, rb, (long)sizeof rb)) <= 0 || (i = 0)))
					goto fail;
				if (rb[i] == 1)
					a += 254;
			} while (rb[i++] == 1);
			if (rb[i - 1] == 0)
				break;
			a += rb[i - 1];
		}
	}
	for (i = 0; i < bl; i++)
		p[tl + dl + i] = 0;
	put32(bp + 8, (long)p);
	put32(bp + 12, tl);
	put32(bp + 16, (long)p + tl);
	put32(bp + 20, dl);
	put32(bp + 24, (long)p + tl + dl);
	put32(bp + 28, bl);
	sys_close(fd);
	return (long)bp;
fail:
	sys_close(fd);
	Mfree(get32(bp + 0x2c));
	Mfree(bp);
	return r;
}

/* the drive a call names; with a = 0 the default one */
static int
drv(d)
	int d;
{
	return d ? d - 1 : Dgetdrv();
}

/* a GEMDOS call; 1 if served here, its result in *ret */
int
gemdos(a, ret)
	short *a;
	long *ret;
{
	struct handle *h;
	char *t = 0, *t2;
	int fn = a[0], d1, d2;
	long r;

	switch (fn) {
	case 0x3e: case 0x3f: case 0x40: case 0x42: case 0x57:
		r = (fn == 0x42 || fn == 0x57 ? a[3] : a[1]) - HBASE;
		if (r < 0 || r >= NH || !hd[r].fd)
			return 0;
		h = hd + r;
		switch (fn) {
		case 0x3e:
			sys_close(h->fd - 1);
			h->fd = 0;
			r = 0;
			break;
		case 0x3f:
		case 0x40:
			r = (fn == 0x3f ? sys_read : sys_write)(h->fd - 1, L(a + 4), L(a + 2));
			if (r < 0)
				r = err(r);
			break;
		case 0x42:
			r = sys_lseek(h->fd - 1, L(a + 1), (long)a[4]);
			if (r < 0)
				r = -64;
			break;
		default:
			r = datime(h, (unsigned short *)L(a + 1), a[4]);
		}
		*ret = r;
		return 1;
	case 0x36:
		if (!sel(drv(a[3])))
			return 0;
		*ret = dfree((char *)L(a + 1));
		return 1;
	case 0x47:
		if (!sel(drv(a[3])))
			return 0;
		*ret = dgetpath((char *)L(a + 1));
		return 1;
	case 0x4f:
		return fsnext(ret);
	case 0x4b:
		if (a[1] != 0 && a[1] != 3)
			return 0;
		t = (char *)L(a + 2);
		break;
	case 0x56:
		t = (char *)L(a + 2);
		t2 = (char *)L(a + 4);
		if (!t || !t2)
			return 0;
		d1 = drive(t, &t);
		d2 = drive(t2, &t2);
		if (!sel(d2) && !sel(d1))
			return 0;
		if (d1 != d2)
			*ret = E_NSAME;
		else if (cur->ro)
			*ret = E_ACCDN;
		else if ((r = resolve(t, 1)) != 0)
			*ret = r < 0 ? r : E_FILNF;
		else if (rel[0] == 0)
			*ret = E_ACCDN;
		else {
			scpy(dp, hp);
			r = resolve(t2, 1);
			*ret = r == 0 ? E_ACCDN : r < 0 ? r : sc(sys_rename(dp, hp));
		}
		return 1;
	case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x41: case 0x43: case 0x4e:
		t = (char *)L(a + 1);
		break;
	default:
		return 0;
	}
	if (!t || !sel(drive(t, &t)))
		return 0;
	if (cur->ro && (fn == 0x39 || fn == 0x3a || fn == 0x3c || fn == 0x41 ||
	    (fn == 0x3d && (a[3] & 3)) || (fn == 0x43 && a[3]))) {
		*ret = E_ACCDN;
		return 1;
	}
	switch (fn) {
	case 0x39:
		r = resolve(t, 0);
		r = r == 0 ? E_ACCDN : r < 0 ? r : sc(sys_mkdir(hp, 0777L));
		break;
	case 0x3a:
		r = resolve(t, 1) ? E_PTHNF : rel[0] == 0 ? E_ACCDN : sc(sys_rmdir(hp));
		break;
	case 0x3b:
		r = resolve(t, 0) || _xstat(2, hp, &st) < 0 || !isdir() ? E_PTHNF : 0;
		if (r == 0)
			scpy(cur->cwd, rel);
		break;
	case 0x3c:
		r = fopen(t, 2, 1, a[3]);
		break;
	case 0x3d:
		r = fopen(t, a[3], 0, 0);
		break;
	case 0x41:
		r = resolve(t, 1);
		r = r ? E_FILNF : _lxstat(2, hp, &st) == 0 && isdir() ? E_ACCDN : sc(sys_unlink(hp));
		break;
	case 0x43:
		r = fattrib(t, a[3], a[4]);
		break;
	case 0x4b:
		r = pexec(t, (char *)L(a + 4), (char *)L(a + 6));
		if (r > 0 && a[1] == 0) {
			/* TOS runs it as Pexec(6, 0, basepage, 0), on the caller's own stack */
			a[1] = 6;
			a[2] = a[3] = a[6] = a[7] = 0;
			a[4] = r >> 16;
			a[5] = r;
			return 0;
		}
		break;
	default:
		r = fsfirst(t, a[3]);
	}
	*ret = r;
	return 1;
}

/* at reset: what a previous run left open; the drives, as a Drvmap mask */
long
hinit()
{
	char *p;
	long m = 0;
	int i, d;

	for (i = 0; i < NH; i++)
		if (hd[i].fd)
			sys_close(hd[i].fd - 1);
	for (i = 0; i < NS; i++)
		if (sr[i].dta)
			sys_close(sr[i].fd);
	for (i = 0; i < sizeof hd; i++)
		((char *)hd)[i] = 0;
	for (i = 0; i < sizeof sr; i++)
		((char *)sr)[i] = 0;
	for (i = 0; i < sizeof dmap; i++)
		dmap[i] = 0;
	for (i = 0, p = p_dtab; *p && i < NDRV; i++) {
		d = up(*p) - 'A';
		dv[i].ro = p[1] & 1;
		dv[i].root = p += 2;
		dv[i].cwd[0] = 0;
		p += slen(p) + 1;
		if (d >= 0 && d < 26) {
			dmap[d] = i + 1;
			m |= 1L << d;
		}
	}
	return m;
}
