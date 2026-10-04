/*
 * mkaslm -- read, check and build 68k libraries for the Shared Library
 * Manager.  A library is a 'libr' descriptor, an optional 'libi' import
 * list and a code resource type: 0 the jump table, 1 %A5Init, 2 Main.
 *
 *	mkaslm -d file		describe every library in the file
 *	mkaslm -t file		re-emit each library resource from its parsed
 *				form and compare; rebuild the whole fork
 *	mkaslm -l file		lay the libraries out as the manager does
 *				and check jump tables, relocations, imports
 *				and (ours) export records
 *	mkaslm -x file [id]	(68k hosts) load the libraries (or the one
 *				named) into memory, run their entry and
 *				exported functions
 *	mkaslm [options] -o out obj
 *				build from an m68k ELF relocatable:
 *	  -n name	library id (default AUXLib$lib)
 *	  -v ver	library version (0x0110)
 *	  -c type	code resource type (cdAU)
 *	  -i id		'libr' id (128)
 *	  -F f -G g	the two 'libr' flag words (0x0840, 0x001c)
 *	  -e spec	export a function set by name:
 *			id,ver,minver,sym,sym... (repeatable, up to 8)
 *	  -u spec	the same, exported by index only
 *	  -C creator	file creator (AUXn)
 *	  -f fmt	bin (MacBinary, default), ad (AppleDouble), rsrc
 *
 * Files are read as MacBinary, AppleDouble or a bare resource fork.
 * All formats are read by byte offsets.  K&R C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	U32(x)	((unsigned long)(x) & 0xffffffffUL)
#define	S32(x)	((long)(U32(x) ^ 0x80000000UL) - 0x7fffffffL - 1)

static char *prog = "mkaslm";

static void
die(s, a)
	char *s, *a;
{
	fprintf(stderr, "%s: ", prog);
	fprintf(stderr, s, a);
	fputc('\n', stderr);
	exit(2);
}

static char *
xalloc(n)
	long n;
{
	char *p = calloc(1, (size_t)(n > 0 ? n : 1));

	if (p == 0)
		die("out of memory", "");
	return p;
}

static unsigned long
rd32(p)
	unsigned char *p;
{
	return U32((unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3]);
}

static unsigned
rd16(p)
	unsigned char *p;
{
	return p[0] << 8 | p[1];
}

static void
wr32(p, v)
	unsigned char *p;
	unsigned long v;
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void
wr16(p, v)
	unsigned char *p;
	unsigned v;
{
	p[0] = v >> 8; p[1] = v;
}

/* ---- growable byte buffer ---- */

struct buf {
	unsigned char	*p;
	long		n, cap;
};

static void
bneed(b, n)
	struct buf *b;
	long n;
{
	if (b->n + n <= b->cap)
		return;
	b->cap = (b->n + n) * 2 + 256;
	b->p = (unsigned char *)realloc(b->p, (size_t)b->cap);
	if (b->p == 0)
		die("out of memory", "");
}

static void
bput(b, p, n)
	struct buf *b;
	unsigned char *p;
	long n;
{
	bneed(b, n);
	if (p)
		memcpy(b->p + b->n, p, (size_t)n);
	else
		memset(b->p + b->n, 0, (size_t)n);
	b->n += n;
}

static void
b8(b, v)
	struct buf *b;
	unsigned v;
{
	unsigned char c = v;

	bput(b, &c, 1L);
}

static void
b16(b, v)
	struct buf *b;
	unsigned v;
{
	unsigned char c[2];

	wr16(c, v);
	bput(b, c, 2L);
}

static void
b32(b, v)
	struct buf *b;
	unsigned long v;
{
	unsigned char c[4];

	wr32(c, v);
	bput(b, c, 4L);
}

static void
beven(b)
	struct buf *b;
{
	if (b->n & 1)
		b8(b, 0);
}

static unsigned char *
readfile(fn, np)
	char *fn;
	long *np;
{
	FILE *f = fopen(fn, "rb");
	unsigned char *p;
	long n;

	if (f == 0)
		die("cannot open %s", fn);
	fseek(f, 0L, 2);
	if ((n = ftell(f)) < 0 || n > 0x7ffffff0L)
		die("cannot size %s", fn);
	fseek(f, 0L, 0);
	p = (unsigned char *)xalloc(n + 1);
	if (fread(p, 1, (size_t)n, f) != (size_t)n)
		die("read error on %s", fn);
	fclose(f);
	*np = n;
	return p;
}

static void
writefile(fn, p, n)
	char *fn;
	unsigned char *p;
	long n;
{
	FILE *f = fopen(fn, "wb");

	if (f == 0 || fwrite(p, 1, (size_t)n, f) != (size_t)n || fclose(f))
		die("cannot write %s", fn);
}

/* ---- containers ---- */

struct file {
	unsigned char	*rf;		/* resource fork */
	long		rlen;
	unsigned char	type[4], creator[4];
};

static void
loadfile(fn, f)
	char *fn;
	struct file *f;
{
	long n, i, k;
	unsigned long off, len, dl, rl, dpad;
	unsigned char *p = readfile(fn, &n);

	memset(f, 0, sizeof *f);
	if (n >= 26 && (rd32(p) == 0x00051600UL || rd32(p) == 0x00051607UL)) {
		k = rd16(p + 24);
		for (i = 0; i < k && 26 + 12 * i + 12 <= n; i++) {
			off = rd32(p + 26 + 12 * i + 4);
			len = rd32(p + 26 + 12 * i + 8);
			if (off > (unsigned long)n || len > (unsigned long)n - off)
				die("%s: bad AppleDouble entry", fn);
			if (rd32(p + 26 + 12 * i) == 2) {
				f->rf = p + off;
				f->rlen = len;
			} else if (rd32(p + 26 + 12 * i) == 9 && len >= 8) {
				memcpy(f->type, p + off, 4);
				memcpy(f->creator, p + off + 4, 4);
			}
		}
		return;
	}
	if (n >= 128 && p[0] == 0 && p[74] == 0 && p[1] >= 1 && p[1] <= 63) {
		dl = rd32(p + 83);
		rl = rd32(p + 87);
		dpad = (dl + 127) & ~127UL;
		if (dl <= (unsigned long)n && dpad <= (unsigned long)n - 128 &&
		    rl <= (unsigned long)n - 128 - dpad) {
			memcpy(f->type, p + 65, 4);
			memcpy(f->creator, p + 69, 4);
			f->rf = p + 128 + dpad;
			f->rlen = rl;
			return;
		}
	}
	f->rf = p;
	f->rlen = n;
}

/* CRC-16/XMODEM, as MacBinary II uses */
static unsigned
crc16(p, n)
	unsigned char *p;
	int n;
{
	unsigned c = 0;
	int i;

	while (n-- > 0) {
		c ^= *p++ << 8;
		for (i = 0; i < 8; i++)
			c = c & 0x8000 ? (c << 1 ^ 0x1021) : c << 1;
		c &= 0xffff;
	}
	return c;
}

static void
savefile(fn, fmt, name, type, creator, rf)
	char *fn, *fmt, *name, *type, *creator;
	struct buf *rf;
{
	struct buf o;
	unsigned char h[128];
	int nl = strlen(name) > 63 ? 63 : strlen(name);

	memset(&o, 0, sizeof o);
	if (strcmp(fmt, "rsrc") == 0) {
		writefile(fn, rf->p, rf->n);
		return;
	}
	if (strcmp(fmt, "ad") == 0) {
		b32(&o, 0x00051607UL);
		b32(&o, 0x00020000UL);
		bput(&o, (unsigned char *)0, 16L);
		b16(&o, 2);
		b32(&o, 9L); b32(&o, 50L); b32(&o, 32L);
		b32(&o, 2L); b32(&o, 82L); b32(&o, (unsigned long)rf->n);
		bput(&o, (unsigned char *)type, 4L);
		bput(&o, (unsigned char *)creator, 4L);
		bput(&o, (unsigned char *)0, 24L);
		bput(&o, rf->p, rf->n);
		writefile(fn, o.p, o.n);
		return;
	}
	if (strcmp(fmt, "bin") != 0)
		die("unknown format %s", fmt);
	memset(h, 0, sizeof h);
	h[1] = nl;
	memcpy(h + 2, name, (size_t)nl);
	memcpy(h + 65, type, 4);
	memcpy(h + 69, creator, 4);
	wr32(h + 87, (unsigned long)rf->n);
	h[122] = h[123] = 129;
	wr16(h + 124, crc16(h, 124));
	bput(&o, h, 128L);
	bput(&o, rf->p, rf->n);
	bput(&o, (unsigned char *)0, (128 - rf->n % 128) % 128);
	writefile(fn, o.p, o.n);
}

/* ---- resource fork ---- */

struct res {
	unsigned char	type[4];
	int		id, attr;
	unsigned char	*name;		/* Pascal string, or 0 */
	unsigned char	*data;
	long		len;
	long		doff, noff;	/* as read: data and name order */
	int		tord;		/* as read: type order */
	unsigned char	hnd[4];		/* handle field as read */
};

struct fork {
	struct res	*r;
	int		n;
	long		dataoff;
	unsigned char	pre[256];	/* bytes before the data */
	unsigned char	mhead[24];	/* map: header copy, handle, refnum */
	int		mattr;
	unsigned char	*post;		/* bytes after the map */
	long		plen;
};

static void
parsefork(p, n, fk)
	unsigned char *p;
	long n;
	struct fork *fk;
{
	unsigned long doff, moff, mlen, dlen, tl, nl, nt, i, j, k, c, ro, e, d;
	unsigned char *m;
	struct res *r;

	memset(fk, 0, sizeof *fk);
	if (n < 16)
		die("no resource fork", "");
	doff = rd32(p);
	moff = rd32(p + 4);
	dlen = rd32(p + 8);
	mlen = rd32(p + 12);
	if (doff > 256 || doff > (unsigned long)n || dlen > (unsigned long)n - doff ||
	    moff > (unsigned long)n || mlen > (unsigned long)n - moff || mlen < 30)
		die("bad resource fork header", "");
	fk->dataoff = doff;
	fk->post = p + moff + mlen;
	fk->plen = n - moff - mlen;
	memcpy(fk->pre, p, (size_t)doff);
	m = p + moff;
	memcpy(fk->mhead, m, 24);
	fk->mattr = rd16(m + 22);
	tl = rd16(m + 24);
	nl = rd16(m + 26);
	if (tl + 2 > mlen || nl > mlen)
		die("bad resource map", "");
	nt = (rd16(m + tl) + 1) & 0xffff;
	if (nt * 8 > mlen - tl - 2)
		die("resource type list out of range", "");
	for (i = 0, k = 0; i < nt; i++)
		k += rd16(m + tl + 2 + 8 * i + 4) + 1;
	if (k > mlen / 12)
		die("more resources than the map holds", "");
	fk->r = (struct res *)xalloc((long)k * (long)sizeof(struct res));
	for (i = 0; i < nt; i++) {
		c = rd16(m + tl + 2 + 8 * i + 4) + 1;
		ro = rd16(m + tl + 2 + 8 * i + 6);
		for (j = 0; j < c; j++) {
			r = &fk->r[fk->n++];
			memcpy(r->type, m + tl + 2 + 8 * i, 4);
			e = tl + ro + 12 * j;
			if (e + 12 > mlen)
				die("resource reference out of range", "");
			r->id = (short)rd16(m + e);
			r->noff = rd16(m + e + 2);
			r->attr = m[e + 4];
			d = rd32(m + e + 4) & 0xffffff;
			r->doff = d;
			if (d + 4 > (unsigned long)n - doff)
				die("resource data out of range", "");
			r->len = rd32(p + doff + d);
			if ((unsigned long)r->len > (unsigned long)n - doff - d - 4)
				die("resource length out of range", "");
			r->data = p + doff + d + 4;
			r->name = 0;
			if (r->noff != 0xffff) {
				if (nl + r->noff >= mlen || nl + r->noff + 1 + m[nl + r->noff] > mlen)
					die("resource name out of range", "");
				r->name = m + nl + r->noff;
			}
			r->tord = i;
			memcpy(r->hnd, m + e + 8, 4);
		}
	}
}

static int
bydoff(a, b)
	char *a, *b;
{
	long x = (*(struct res **)a)->doff, y = (*(struct res **)b)->doff;

	return x < y ? -1 : x > y;
}

static int
bynoff(a, b)
	char *a, *b;
{
	long x = (*(struct res **)a)->noff, y = (*(struct res **)b)->noff;

	return x < y ? -1 : x > y;
}

/* Write the fork: data in read order, types and references in read order. */
static void
buildfork(fk, o)
	struct fork *fk;
	struct buf *o;
{
	struct res **v = (struct res **)xalloc((fk->n + 1) * (long)sizeof *v);
	struct buf m, names;
	long *dpos = (long *)xalloc((fk->n + 1) * (long)sizeof(long));
	long *npos = (long *)xalloc((fk->n + 1) * (long)sizeof(long));
	int i, j, t, nt, cnt, first;
	long dl, tl, rl;

	memset(&m, 0, sizeof m);
	memset(&names, 0, sizeof names);
	o->n = 0;
	bput(o, fk->pre, fk->dataoff);
	for (i = 0; i < fk->n; i++)
		v[i] = &fk->r[i];
	qsort((char *)v, (size_t)fk->n, sizeof *v, bydoff);
	for (i = 0; i < fk->n; i++) {
		dpos[v[i] - fk->r] = o->n - fk->dataoff;
		b32(o, (unsigned long)v[i]->len);
		bput(o, v[i]->data, v[i]->len);
	}
	dl = o->n - fk->dataoff;
	for (i = 0; i < fk->n; i++)
		v[i] = &fk->r[i];
	qsort((char *)v, (size_t)fk->n, sizeof *v, bynoff);
	for (i = 0; i < fk->n; i++) {
		npos[v[i] - fk->r] = -1;
		if (v[i]->name) {
			npos[v[i] - fk->r] = names.n;
			bput(&names, v[i]->name, (long)v[i]->name[0] + 1);
		}
	}
	for (nt = 0, i = 0; i < fk->n; i++)
		if (fk->r[i].tord + 1 > nt)
			nt = fk->r[i].tord + 1;
	bput(&m, fk->mhead, 22L);
	b16(&m, (unsigned)fk->mattr);
	b16(&m, 28);
	b16(&m, 0);			/* name list offset, below */
	tl = m.n;
	b16(&m, (unsigned)(nt - 1));
	rl = 2 + 8L * nt;
	for (t = 0; t < nt; t++) {
		for (cnt = 0, first = -1, i = 0; i < fk->n; i++)
			if (fk->r[i].tord == t) {
				if (first < 0)
					first = i;
				cnt++;
			}
		bput(&m, fk->r[first].type, 4L);
		b16(&m, (unsigned)(cnt - 1));
		b16(&m, (unsigned)rl);
		rl += 12L * cnt;
	}
	for (t = 0; t < nt; t++)
		for (i = 0; i < fk->n; i++) {
			if (fk->r[i].tord != t)
				continue;
			b16(&m, (unsigned)fk->r[i].id & 0xffff);
			b16(&m, npos[i] < 0 ? 0xffff : (unsigned)npos[i]);
			b32(&m, (unsigned long)fk->r[i].attr << 24 | dpos[i]);
			bput(&m, fk->r[i].hnd, 4L);
		}
	wr16(m.p + 26, (unsigned)m.n);
	bput(&m, names.p, names.n);
	j = (int)o->n;
	wr32(o->p, (unsigned long)fk->dataoff);
	wr32(o->p + 4, (unsigned long)j);
	wr32(o->p + 8, (unsigned long)dl);
	wr32(o->p + 12, (unsigned long)m.n);
	if (fk->dataoff >= 16 && memcmp(fk->mhead, fk->pre, 16) == 0)
		memcpy(m.p, o->p, 16);
	bput(o, m.p, m.n);
	bput(o, fk->post, fk->plen);
	(void)tl;
	free(v); free(dpos); free(npos); free(m.p); free(names.p);
}

static struct res *
findres(fk, type, id)
	struct fork *fk;
	char *type;
	int id;
{
	int i;

	for (i = 0; i < fk->n; i++)
		if (memcmp(fk->r[i].type, type, 4) == 0 && fk->r[i].id == id)
			return &fk->r[i];
	return 0;
}

/* ---- 'libr' ---- */

struct lclass {
	unsigned long	kind;		/* 4 function set, 0x100 class, ... */
	unsigned	ver, minver;
	char		*name;
	int		np;
	char		**par;		/* parent class ids */
};

struct libr {
	char		*name;
	unsigned char	code[4];
	unsigned	fmt, ver;
	unsigned long	x;
	unsigned char	z6[6];
	unsigned	f1, f2;
	int		nc;
	struct lclass	*c;
};

/* C string at *op, padded to even; 0 if it runs off the end */
static char *
cstr(p, n, op)
	unsigned char *p;
	long n, *op;
{
	long o = *op, z;

	for (z = o; z < n && p[z]; z++)
		;
	if (z >= n)
		return 0;
	*op = (z + 2) & ~1L;
	return (char *)p + o;
}

static int
parselibr(p, n, l)
	unsigned char *p;
	long n;
	struct libr *l;
{
	long o = 0;
	int i, j;
	struct lclass *c;

	memset(l, 0, sizeof *l);
	if ((l->name = cstr(p, n, &o)) == 0 || o + 26 > n)
		return -1;
	memcpy(l->code, p + o, 4);
	l->fmt = rd16(p + o + 4);
	l->ver = rd16(p + o + 6);
	l->x = rd32(p + o + 8);
	memcpy(l->z6, p + o + 12, 6);
	l->f1 = rd16(p + o + 18);
	l->f2 = rd16(p + o + 20);
	l->nc = rd16(p + o + 22);
	o += 24;
	l->c = (struct lclass *)xalloc((l->nc + 1) * (long)sizeof *c);
	for (i = 0; i < l->nc; i++) {
		c = &l->c[i];
		if (o + 8 > n)
			return -1;
		c->kind = rd32(p + o);
		c->ver = rd16(p + o + 4);
		c->minver = rd16(p + o + 6);
		o += 8;
		if ((c->name = cstr(p, n, &o)) == 0 || o + 2 > n)
			return -1;
		c->np = rd16(p + o);
		o += 2;
		c->par = (char **)xalloc((c->np + 1) * (long)sizeof(char *));
		for (j = 0; j < c->np; j++)
			if ((c->par[j] = cstr(p, n, &o)) == 0)
				return -1;
	}
	return o == n ? 0 : -1;
}

static void
bcstr(b, s)
	struct buf *b;
	char *s;
{
	bput(b, (unsigned char *)s, (long)strlen(s) + 1);
	beven(b);
}

static void
emitlibr(l, b)
	struct libr *l;
	struct buf *b;
{
	int i, j;

	bcstr(b, l->name);
	bput(b, l->code, 4L);
	b16(b, l->fmt);
	b16(b, l->ver);
	b32(b, l->x);
	bput(b, l->z6, 6L);
	b16(b, l->f1);
	b16(b, l->f2);
	b16(b, (unsigned)l->nc);
	for (i = 0; i < l->nc; i++) {
		b32(b, l->c[i].kind);
		b16(b, l->c[i].ver);
		b16(b, l->c[i].minver);
		bcstr(b, l->c[i].name);
		b16(b, (unsigned)l->c[i].np);
		for (j = 0; j < l->c[i].np; j++)
			bcstr(b, l->c[i].par[j]);
	}
}

/* ---- 'libi': two words, a count, then A5 offsets of import records ---- */

struct libi {
	unsigned long	a, b;
	long		n, *off;
};

static int
parselibi(p, n, li)
	unsigned char *p;
	long n;
	struct libi *li;
{
	long i;

	if (n < 12)
		return -1;
	li->a = rd32(p);
	li->b = rd32(p + 4);
	li->n = rd32(p + 8);
	if ((unsigned long)li->n > (unsigned long)(n - 12) / 4 || 12 + 4 * li->n != n)
		return -1;
	li->off = (long *)xalloc((li->n + 1) * (long)sizeof(long));
	for (i = 0; i < li->n; i++)
		li->off[i] = S32(rd32(p + 12 + 4 * i));
	return 0;
}

static void
emitlibi(li, b)
	struct libi *li;
	struct buf *b;
{
	long i;

	b32(b, li->a);
	b32(b, li->b);
	b32(b, (unsigned long)li->n);
	for (i = 0; i < li->n; i++)
		b32(b, U32(li->off[i]));
}

/* ---- code resources ---- */

/*
 * Relocation list: offsets from the resource start, each coded as the
 * distance from the previous one in words: 1 byte (1..7f), 2 bytes
 * (80|hi, lo) or 00 + 4 bytes (first byte nonzero); 00 00 ends it.
 */
struct rel {
	long	n, *off;
};

static long
reldecode(p, n, at, r)
	unsigned char *p;
	long n, at;
	struct rel *r;
{
	long i = at, pos = 0, d, cap = 16;

	r->n = 0;
	r->off = (long *)xalloc(cap * (long)sizeof(long));
	for (;;) {
		if (i >= n)
			return -1;
		d = p[i++];
		if (d == 0) {
			if (i >= n)
				return -1;
			if (p[i] == 0)
				return i + 1;
			if (i + 4 > n)
				return -1;
			d = rd32(p + i);
			i += 4;
		} else if (d & 0x80) {
			if (i >= n)
				return -1;
			d = (d & 0x7f) << 8 | p[i++];
		}
		if (d < 0 || d > n - pos / 2)	/* no offset beyond the resource */
			return -1;
		pos += 2 * d;
		if (r->n == cap) {
			cap *= 2;
			r->off = (long *)realloc((char *)r->off,
			    (size_t)cap * sizeof(long));
			if (r->off == 0)
				die("out of memory", "");
		}
		r->off[r->n++] = pos;
	}
}

static int
relencode(r, b)
	struct rel *r;
	struct buf *b;
{
	long i, pos = 0, d;

	for (i = 0; i < r->n; i++) {
		d = r->off[i] - pos;
		if (d <= 0 || (d & 1))
			return -1;
		d >>= 1;
		if (d < 0x80)
			b8(b, (unsigned)d);
		else if (d < 0x8000)
			b16(b, (unsigned)(0x8000 | d));
		else if (d >= 0x1000000) {
			b8(b, 0);
			b32(b, (unsigned long)d);
		} else
			return -1;
		pos = r->off[i];
	}
	b16(b, 0);
	return 0;
}

#define	FARHDR	0x28

struct seg {
	int		far;		/* 0: 4-byte near header */
	unsigned long	h[10];		/* far header longs after the ffff 0000 */
	unsigned char	*code;		/* after the header, up to the lists */
	long		clen;
	struct rel	a5, sr;
	unsigned char	*gap, *tail;	/* between and after the lists */
	long		glen, tlen;
};

/* far header longs */
#define	H_NEAROFF	0
#define	H_NEARCNT	1
#define	H_FAROFF	2
#define	H_FARCNT	3
#define	H_A5REL		4
#define	H_CURA5		5
#define	H_SEGREL	6
#define	H_CURLOAD	7

static int
parseseg(p, n, s)
	unsigned char *p;
	long n;
	struct seg *s;
{
	long e1, e2, i;

	memset(s, 0, sizeof *s);
	if (n < 4)
		return -1;
	if (rd16(p) != 0xffff) {
		s->code = p + 4;
		s->clen = n - 4;
		s->h[0] = rd16(p);
		s->h[1] = rd16(p + 2);
		return 0;
	}
	s->far = 1;
	if (n < FARHDR || rd16(p + 2) != 0)
		return -1;
	for (i = 0; i < 9; i++)
		s->h[i] = rd32(p + 4 + 4 * i);
	if (s->h[H_A5REL] == 0 && s->h[H_SEGREL] == 0) {
		s->code = p + FARHDR;
		s->clen = n - FARHDR;
		return 0;
	}
	if (s->h[H_A5REL] < FARHDR || s->h[H_SEGREL] < s->h[H_A5REL] ||
	    s->h[H_SEGREL] > (unsigned long)n)
		return -1;
	if ((e1 = reldecode(p, n, (long)s->h[H_A5REL], &s->a5)) < 0 ||
	    e1 > (long)s->h[H_SEGREL] ||
	    (e2 = reldecode(p, n, (long)s->h[H_SEGREL], &s->sr)) < 0)
		return -1;
	s->code = p + FARHDR;
	s->clen = s->h[H_A5REL] - FARHDR;
	s->gap = p + e1;
	s->glen = s->h[H_SEGREL] - e1;
	s->tail = p + e2;
	s->tlen = n - e2;
	return 0;
}

static int
emitseg(s, b)
	struct seg *s;
	struct buf *b;
{
	long h0 = b->n, a, i;

	if (!s->far) {
		b16(b, (unsigned)s->h[0]);
		b16(b, (unsigned)s->h[1]);
		bput(b, s->code, s->clen);
		return 0;
	}
	b16(b, 0xffff);
	b16(b, 0);
	for (i = 0; i < 9; i++)
		b32(b, s->h[i]);
	bput(b, s->code, s->clen);
	if (s->a5.n == 0 && s->sr.n == 0 && s->gap == 0) {
		wr32(b->p + h0 + 4 + 4 * H_A5REL, 0L);
		wr32(b->p + h0 + 4 + 4 * H_SEGREL, 0L);
		return 0;
	}
	a = b->n - h0;
	wr32(b->p + h0 + 4 + 4 * H_A5REL, (unsigned long)a);
	if (relencode(&s->a5, b) < 0)
		return -1;
	bput(b, s->gap, s->glen);
	wr32(b->p + h0 + 4 + 4 * H_SEGREL, (unsigned long)(b->n - h0));
	if (relencode(&s->sr, b) < 0)
		return -1;
	bput(b, s->tail, s->tlen);
	return 0;
}

/* jump table (resource 0) */
struct jt {
	unsigned long	above, below, len, off;
	int		n;
	unsigned char	*e;		/* entries, 8 bytes each */
};

static int
parsejt(p, n, j)
	unsigned char *p;
	long n;
	struct jt *j;
{
	if (n < 16)
		return -1;
	j->above = rd32(p);
	j->below = rd32(p + 4);
	j->len = rd32(p + 8);
	j->off = rd32(p + 12);
	if (j->len > (unsigned long)(n - 16) || j->len % 8 || 16 + j->len != (unsigned long)n)
		return -1;
	j->n = j->len / 8;
	j->e = p + 16;
	return 0;
}

/* far entry k: segment and resource offset, or -1 if near/marker */
static int
jtfar(j, k, off)
	struct jt *j;
	int k;
	long *off;
{
	unsigned char *e = j->e + 8 * k;

	if (rd16(e + 2) != 0xa9f0)
		return -1;
	*off = rd32(e + 4);
	return rd16(e);
}

/* ---- one library: its descriptor and resources ---- */

struct lib {
	struct res	*lr, *li, *r0, *seg[16];
	struct libr	l;
	struct libi	i;
	struct jt	jt;
	struct seg	s[16];
	int		nseg;
};

static int
getlib(fk, lr, lb, why)
	struct fork *fk;
	struct res *lr;
	struct lib *lb;
	char **why;
{
	int k;

	memset(lb, 0, sizeof *lb);
	lb->lr = lr;
	if (parselibr(lr->data, lr->len, &lb->l) < 0)
		return *why = "libr does not parse", -1;
	lb->li = findres(fk, "libi", lr->id);
	if (lb->li && parselibi(lb->li->data, lb->li->len, &lb->i) < 0)
		return *why = "libi does not parse", -1;
	if ((lb->r0 = findres(fk, (char *)lb->l.code, 0)) == 0)
		return *why = "no jump table resource", -1;
	if (parsejt(lb->r0->data, lb->r0->len, &lb->jt) < 0)
		return *why = "bad jump table", -1;
	for (k = 1; k < 16 && (lb->seg[k] = findres(fk, (char *)lb->l.code, k)); k++)
		if (parseseg(lb->seg[k]->data, lb->seg[k]->len, &lb->s[k]) < 0)
			return *why = "segment does not parse", -1;
	lb->nseg = k;
	return 0;
}

static char *
t4(p)
	unsigned char *p;
{
	static char s[4][5];
	static int k;
	int i;

	k = (k + 1) & 3;
	for (i = 0; i < 4; i++)
		s[k][i] = p[i] >= 32 && p[i] < 127 ? p[i] : '?';
	s[k][4] = 0;
	return s[k];
}

/* ---- -d ---- */

static int
describe(fk)
	struct fork *fk;
{
	int i, j, k, far;
	struct lib lb;
	char *why;
	long off;

	for (i = 0; i < fk->n; i++) {
		if (memcmp(fk->r[i].type, "libr", 4) != 0)
			continue;
		if (getlib(fk, &fk->r[i], &lb, &why) < 0) {
			printf("libr %d: %s\n", fk->r[i].id, why);
			continue;
		}
		printf("libr %d %s code '%s' ver %04x flags %04x %04x\n",
		    fk->r[i].id, lb.l.name, t4(lb.l.code), lb.l.ver,
		    lb.l.f1, lb.l.f2);
		for (j = 0; j < lb.l.nc; j++) {
			printf("  %s %s %04x/%04x",
			    lb.l.c[j].kind == 4 ? "fset " : "class",
			    lb.l.c[j].name, lb.l.c[j].ver, lb.l.c[j].minver);
			for (k = 0; k < lb.l.c[j].np; k++)
				printf(" : %s", lb.l.c[j].par[k]);
			printf("\n");
		}
		for (far = 0, k = 0; k < lb.jt.n; k++)
			if (jtfar(&lb.jt, k, &off) >= 0)
				far++;
		printf("  A5 world -%#lx..+%#lx, %d jump table entries (%d far)",
		    lb.jt.below, lb.jt.above, lb.jt.n, far);
		if (lb.li)
			printf(", %ld imports", lb.i.n);
		printf("\n");
		for (k = 1; k < lb.nseg; k++)
			printf("  seg %d %-8.*s %6ld bytes code, %ld A5 + %ld segment relocations\n",
			    k, lb.seg[k]->name ? lb.seg[k]->name[0] : 0,
			    lb.seg[k]->name ? (char *)lb.seg[k]->name + 1 : "",
			    lb.s[k].clen, lb.s[k].a5.n, lb.s[k].sr.n);
	}
	return 0;
}

/* ---- -t ---- */

static int
same(what, id, a, an, b)
	char *what;
	int id;
	unsigned char *a;
	long an;
	struct buf *b;
{
	long i;

	if (an == b->n && memcmp(a, b->p, (size_t)an) == 0)
		return 1;
	for (i = 0; i < an && i < b->n && a[i] == b->p[i]; i++)
		;
	printf("DIFF %s %d: %ld vs %ld bytes, first at %ld\n", what, id, an, b->n, i);
	return 0;
}

static int
roundtrip(fk, raw, rawn)
	struct fork *fk;
	unsigned char *raw;
	long rawn;
{
	int i, k, bad = 0, nl = 0, nr = 0;
	struct lib lb;
	struct buf b;
	char *why;

	memset(&b, 0, sizeof b);
	for (i = 0; i < fk->n; i++) {
		if (memcmp(fk->r[i].type, "libr", 4) != 0)
			continue;
		if (getlib(fk, &fk->r[i], &lb, &why) < 0) {
			printf("FAIL libr %d: %s\n", fk->r[i].id, why);
			bad++;
			continue;
		}
		nl++;
		b.n = 0;
		emitlibr(&lb.l, &b);
		bad += !same("libr", lb.lr->id, lb.lr->data, lb.lr->len, &b);
		nr++;
		if (lb.li) {
			b.n = 0;
			emitlibi(&lb.i, &b);
			bad += !same("libi", lb.li->id, lb.li->data, lb.li->len, &b);
			nr++;
		}
		b.n = 0;
		bput(&b, lb.r0->data, 16L);
		bput(&b, lb.jt.e, lb.jt.len);
		bad += !same(t4(lb.l.code), 0, lb.r0->data, lb.r0->len, &b);
		nr++;
		for (k = 1; k < lb.nseg; k++) {
			b.n = 0;
			if (emitseg(&lb.s[k], &b) < 0) {
				printf("FAIL %s %d: relocations do not encode\n",
				    t4(lb.l.code), k);
				bad++;
				continue;
			}
			bad += !same(t4(lb.l.code), k, lb.seg[k]->data,
			    lb.seg[k]->len, &b);
			nr++;
		}
	}
	b.n = 0;
	buildfork(fk, &b);
	if (!same("fork", 0, raw, rawn, &b))
		bad++;
	printf("%s: %d libraries, %d resources re-emitted, fork %s\n",
	    bad ? "FAIL" : "PASS", nl, nr, bad ? "checked" : "identical");
	free(b.p);
	return bad != 0;
}

/* ---- -l: lay out as the manager does and check ---- */

#define	BLKMAGIC	0x41584c31UL	/* "AXL1": our %A5Init data block */

struct blk {
	long		at;		/* offset in segment 1 */
	unsigned long	below, initlen, nrel, nsets;
	unsigned char	*p;
};

static int
findblk(lb, bk)
	struct lib *lb;
	struct blk *bk;
{
	struct seg *s = &lb->s[1];
	long i;

	if (lb->nseg < 2 || !s->far)
		return 0;
	for (i = s->clen - 20; i >= 0; i -= 2)
		if (rd32(s->code + i) == BLKMAGIC)
			break;
	if (i < 0)
		return 0;
	bk->at = i;
	bk->p = s->code + i;
	bk->below = rd32(bk->p + 4);
	bk->initlen = rd32(bk->p + 8);
	bk->nrel = rd32(bk->p + 12);
	bk->nsets = rd32(bk->p + 16);
	i = s->clen - i - 20;			/* bytes after the header */
	if (bk->nsets > 8 || bk->nrel > (unsigned long)i / 4 ||
	    8 * bk->nsets + 4 * bk->nrel > (unsigned long)i ||
	    bk->initlen > (unsigned long)i - 8 * bk->nsets - 4 * bk->nrel ||
	    bk->below > 0x1000000UL)
		return -1;
	return 1;
}

static int nerr, nwarn;

static void
bad(lb, s, a)
	struct lib *lb;
	char *s;
	long a;
{
	printf("  ERROR %s: ", lb->l.name);
	printf(s, a);
	printf("\n");
	nerr++;
}

/* A5-relative value v points at a jump table entry's JMP; its target */
static int
jtref(lb, v, segp, offp)
	struct lib *lb;
	long v, *offp;
	int *segp;
{
	long k = v - (long)lb->jt.off - 2;

	if (k < 0 || k % 8 || k / 8 >= lb->jt.n)
		return -1;
	*segp = jtfar(&lb->jt, (int)(k / 8), offp);
	return *segp < 1 || *segp >= lb->nseg ? -1 : 0;
}

static int
codeok(lb, sg, off)
	struct lib *lb;
	int sg;
	long off;
{
	return sg >= 1 && sg < lb->nseg && !(off & 1) && off >= FARHDR &&
	    off < FARHDR + lb->s[sg].clen;
}

static void
checkours(lb, bk)
	struct lib *lb;
	struct blk *bk;
{
	unsigned char *img = (unsigned char *)xalloc((long)bk->below + 4);
	unsigned char *rp = bk->p + 20 + 8 * bk->nsets, *ip;
	long base = -(long)bk->below, i, j, ro, dof, v, off, nt, nf;
	int sg, set;
	char *nm;

	if (bk->below != lb->jt.below || bk->initlen > bk->below)
		bad(lb, "data block size %#lx disagrees with the jump table", (long)bk->below);
	ip = rp + 4 * bk->nrel;
	memcpy(img, ip, (size_t)bk->initlen);
	for (i = 0; i < (long)bk->nrel; i++) {
		off = rd32(rp + 4 * i);
		if (off < 0 || off + 4 > (long)bk->below || (off & 1))
			bad(lb, "data relocation at %#lx outside the A5 world", off);
	}
	if (bk->nsets != (unsigned long)lb->l.nc)
		bad(lb, "%ld exported sets in the data block", (long)bk->nsets);
	for (set = 0; set < (int)bk->nsets && set < lb->l.nc; set++) {
		ro = rd32(bk->p + 20 + 8 * set);
		dof = rd32(bk->p + 24 + 8 * set);
		if (ro + 17 > (long)bk->below || dof + 8 > (long)bk->below) {
			bad(lb, "export record %d out of range", (long)set);
			continue;
		}
		nm = (char *)img + ro + 16;
		if (rd16(img + ro + 12) != lb->l.c[set].ver ||
		    rd16(img + ro + 14) != lb->l.c[set].minver ||
		    strcmp(nm, lb->l.c[set].name) != 0)
			bad(lb, "export record %d does not match the libr class", (long)set);
		/* descriptor: name table (A5-relative) or 0, functions, 0 */
		nt = S32(rd32(img + dof));
		for (nf = 0, j = dof + 4; j + 4 <= (long)bk->below && (v = S32(rd32(img + j))) != 0; j += 4, nf++)
			if (jtref(lb, v, &sg, &off) < 0 || !codeok(lb, sg, off))
				bad(lb, "exported function %ld is no valid jump table entry", nf);
		if (nt) {
			for (i = 0; ; i++) {
				v = nt - base + 4 * i;
				if (v < 0 || v + 4 > (long)bk->below) {
					bad(lb, "name table out of range", 0L);
					break;
				}
				if ((v = S32(rd32(img + v))) == 0)
					break;
				if (v - base < 0 || v - base >= (long)bk->below)
					bad(lb, "name %ld out of range", i);
			}
			if (i != nf)
				bad(lb, "%ld names for the functions", i);
		}
		printf("  export %s: %ld functions%s\n", lb->l.c[set].name, nf,
		    nt ? " by name" : "");
	}
	free(img);
}

static int
loadcheck(fk)
	struct fork *fk;
{
	int i, k, sg, nl = 0, ours;
	struct lib lb;
	struct blk bk;
	char *why;
	long j, off, v, loc, a5 = 0, jtn = 0;
	struct seg *s;

	nerr = nwarn = 0;
	for (i = 0; i < fk->n; i++) {
		if (memcmp(fk->r[i].type, "libr", 4) != 0)
			continue;
		nl++;
		if (getlib(fk, &fk->r[i], &lb, &why) < 0) {
			printf("  ERROR libr %d: %s\n", fk->r[i].id, why);
			nerr++;
			continue;
		}
		if (lb.jt.off != 0x20 || lb.jt.above != 0x20 + lb.jt.len)
			bad(&lb, "jump table at %#lx", (long)lb.jt.off);
		for (k = 0; k < lb.jt.n; k++) {
			if ((sg = jtfar(&lb.jt, k, &off)) < 0)
				continue;
			if (!codeok(&lb, sg, off))
				bad(&lb, "jump table entry %ld outside its segment", (long)k);
		}
		for (k = 1; k < lb.nseg; k++) {
			s = &lb.s[k];
			if (!s->far) {
				bad(&lb, "segment %ld has a near header", (long)k);
				continue;
			}
			v = s->h[H_NEAROFF] / 8;
			for (j = 0; j < (long)s->h[H_NEARCNT]; j++)
				if (v + j >= lb.jt.n || jtfar(&lb.jt, (int)(v + j), &off) != k)
					bad(&lb, "segment %ld near entries disagree with the table", (long)k);
			for (j = 0; j < s->a5.n; j++) {
				loc = s->a5.off[j] - FARHDR;
				if (loc < 0 || loc + 4 > s->clen) {
					bad(&lb, "A5 relocation at %#lx outside the code", s->a5.off[j]);
					continue;
				}
				v = S32(rd32(s->code + loc));
				if (v < -(long)lb.jt.below || v >= (long)lb.jt.above)
					bad(&lb, "A5 relocation value %#lx outside the A5 world", v);
				else if (v >= (long)lb.jt.off && jtref(&lb, v, &sg, &off) < 0)
					nwarn++;
				a5++;
			}
			for (j = 0; j < s->sr.n; j++) {
				loc = s->sr.off[j] - FARHDR;
				if (loc < 0 || loc + 4 > s->clen) {
					bad(&lb, "segment relocation at %#lx outside the code", s->sr.off[j]);
					continue;
				}
				v = S32(rd32(s->code + loc));
				if (v < 0 || v > s->clen)
					bad(&lb, "segment relocation value %#lx outside the code", v);
			}
		}
		if (lb.li)
			for (j = 0; j < lb.i.n; j++)
				if (lb.i.off[j] >= 0 || lb.i.off[j] < -(long)lb.jt.below)
					bad(&lb, "import record %#lx outside the globals", lb.i.off[j]);
		jtn += lb.jt.n;
		ours = findblk(&lb, &bk);
		if (ours < 0)
			bad(&lb, "data block overruns segment 1", 0L);
		else if (ours)
			checkours(&lb, &bk);
	}
	printf("%s: %d libraries, %ld jump table entries, %ld A5 relocations, "
	    "%d errors, %d A5 references into the table's header\n",
	    nerr ? "FAIL" : "PASS", nl, jtn, a5, nerr, nwarn);
	return nerr != 0;
}

/* ---- -x: run our libraries (68k hosts) ---- */

#if defined(__m68k__) || defined(__mc68000__)
#include "mkaslmx.c"
#else
static int
runlibs(fk, only)
	struct fork *fk;
	char *only;
{
	(void)fk;
	(void)only;
	die("-x runs on 68k hosts only", "");
	return 1;
}
#endif

/* ---- build from ELF ---- */

#define	SHT_SYMTAB	2
#define	SHT_RELA	4
#define	SHT_NOBITS	8
#define	SHT_REL		9
#define	SHF_WRITE	1
#define	SHF_ALLOC	2
#define	SHF_EXEC	4
#define	SHN_UNDEF	0
#define	SHN_ABS		0xfff1
#define	SHN_COMMON	0xfff2
#define	R_68K_NONE	0
#define	R_68K_32	1
#define	R_68K_16	2
#define	R_68K_8		3
#define	R_68K_PC32	4
#define	R_68K_PC16	5
#define	R_68K_PC8	6

#define	SEG_A5INIT	1
#define	SEG_MAIN	2
#define	SEG_DATA	3
#define	SEG_ABS		4

struct esec {
	char		*name;
	unsigned long	type, flags, size, align, link, info;
	unsigned char	*data;
	int		seg;
	long		base;
};

struct esym {
	char		*name;
	unsigned long	value, size;
	int		info, shndx;
	long		cbase;		/* common: offset in the data */
};

struct fset {
	char		*id;
	unsigned	ver, minver;
	int		named, nf;
	char		**fn;
	long		rec, desc, names, ntab;	/* data offsets */
	long		*noff;
};

static struct esec *es;
static struct esym *sy;
static int nes, nsy;
static struct buf segb[3], datab;
static long datalen;			/* with .bss and commons */

/* jump table requests: (segment, code offset) */
static int jtseg[1024];
static long jtoff[1024];
static int njt;

static int
jtindex(sg, off)
	int sg;
	long off;
{
	int i;

	for (i = 0; i < njt; i++)
		if (jtseg[i] == sg && jtoff[i] == off)
			return i;
	if (njt == 1024)
		die("too many jump table entries", "");
	jtseg[njt] = sg;
	jtoff[njt] = off;
	return njt++;
}

static struct rel a5r[3], segr[3], datar;

static void
addrel(r, off)
	struct rel *r;
	long off;
{
	r->off = (long *)realloc((char *)r->off, (size_t)(r->n + 1) * sizeof(long));
	if (r->off == 0)
		die("out of memory", "");
	r->off[r->n++] = off;
}

static int
cmplong(a, b)
	char *a, *b;
{
	long x = *(long *)a, y = *(long *)b;

	return x < y ? -1 : x > y;
}

/* name at off in a string section of size n, or die */
static char *
estr(fn, t, n, off)
	char *fn;
	unsigned char *t;
	unsigned long n, off;
{
	unsigned long i;

	for (i = off; t && i < n; i++)
		if (t[i] == 0)
			return (char *)t + off;
	die("%s: bad name offset", fn);
	return 0;
}

static void
readelf(fn)
	char *fn;
{
	long n, i;
	unsigned long shoff, shstr, off;
	unsigned char *p = readfile(fn, &n), *h;
	int shnum, symsec = -1;
	struct esec *e, *ss, *st;

	if (n < 52 || memcmp(p, "\177ELF\1\2", 6) != 0 || rd16(p + 18) != 4)
		die("%s: not a big-endian m68k ELF32 file", fn);
	if (rd16(p + 16) != 1)
		die("%s: not a relocatable object", fn);
	shoff = rd32(p + 32);
	shnum = rd16(p + 48);
	shstr = rd16(p + 50);
	if (rd16(p + 46) != 40 || shoff > (unsigned long)n ||
	    40UL * shnum > (unsigned long)n - shoff || shstr >= (unsigned long)shnum)
		die("%s: bad section headers", fn);
	nes = shnum;
	es = (struct esec *)xalloc(shnum * (long)sizeof *es);
	for (i = 0; i < shnum; i++) {
		h = p + shoff + 40 * i;
		e = &es[i];
		e->type = rd32(h + 4);
		e->flags = rd32(h + 8);
		e->size = rd32(h + 20);
		e->link = rd32(h + 24);
		e->info = rd32(h + 28);
		e->align = rd32(h + 32);
		if (e->align > 0x10000)
			die("%s: section alignment out of range", fn);
		if (e->type != SHT_NOBITS) {
			off = rd32(h + 16);
			if (off > (unsigned long)n || e->size > (unsigned long)n - off)
				die("%s: section out of range", fn);
			e->data = p + off;
		}
		if (e->type == SHT_SYMTAB)
			symsec = (int)i;
		if (e->type == SHT_REL)
			die("%s: REL relocations are not supported", fn);
	}
	ss = &es[shstr];
	for (i = 0; i < shnum; i++)
		es[i].name = estr(fn, ss->data, ss->size, rd32(p + shoff + 40 * i));
	if (symsec < 0 || es[symsec].link >= (unsigned long)shnum)
		die("%s: no usable symbol table", fn);
	st = &es[es[symsec].link];
	nsy = es[symsec].size / 16;
	sy = (struct esym *)xalloc(nsy * (long)sizeof *sy);
	for (i = 0; i < nsy; i++) {
		h = es[symsec].data + 16 * i;
		sy[i].name = estr(fn, st->data, st->size, rd32(h));
		sy[i].value = rd32(h + 4);
		sy[i].size = rd32(h + 8);
		sy[i].info = h[12];
		sy[i].shndx = rd16(h + 14);
		if (sy[i].shndx >= shnum && sy[i].shndx < 0xff00)
			die("%s: symbol in a missing section", fn);
	}
}

static struct esym *
symbol(name)
	char *name;
{
	int i;

	for (i = 0; i < nsy; i++)
		if ((sy[i].info >> 4) != 0 && strcmp(sy[i].name, name) == 0 &&
		    sy[i].shndx != SHN_UNDEF)
			return &sy[i];
	return 0;
}

/* where symbol+addend lands: segment and offset (code or data) */
static int
target(s, add, offp)
	struct esym *s;
	long add, *offp;
{
	struct esec *e;

	if (s->shndx == SHN_ABS) {
		*offp = s->value + add;
		return SEG_ABS;
	}
	if (s->shndx == SHN_COMMON) {
		*offp = s->cbase + add;
		return SEG_DATA;
	}
	if (s->shndx == SHN_UNDEF) {
		if ((s->info >> 4) == 2) {	/* weak */
			*offp = add;
			return SEG_ABS;
		}
		die("undefined symbol %s (imports are not supported)", s->name);
	}
	if (s->shndx >= nes || (e = &es[s->shndx])->seg == 0)
		die("symbol %s is in a section that is not loaded", s->name);
	*offp = e->base + s->value + add;
	return e->seg;
}

static struct buf *
segbuf(sg)
	int sg;
{
	return sg == SEG_DATA ? &datab : &segb[sg];
}

/* lay out sections; the export area comes first in the data */
static void
layout(fs, nfs)
	struct fset *fs;
	int nfs;
{
	int i, k, pass;
	long a, j;
	struct esec *e;
	struct buf *b;
	struct fset *f;

	for (i = 0; i < nfs; i++) {
		f = &fs[i];
		beven(&datab);
		f->rec = datab.n;
		bput(&datab, (unsigned char *)0, 12L);
		b16(&datab, f->ver);
		b16(&datab, f->minver);
		bcstr(&datab, f->id);
		f->noff = (long *)xalloc((f->nf + 1) * (long)sizeof(long));
		if (f->named) {
			for (k = 0; k < f->nf; k++) {
				f->noff[k] = datab.n;
				bcstr(&datab, f->fn[k]);
			}
			f->ntab = datab.n;
			bput(&datab, (unsigned char *)0, 4L * (f->nf + 1));
		}
		f->desc = datab.n;
		bput(&datab, (unsigned char *)0, 4L * (f->nf + 2));
	}
	for (pass = 0; pass < 3; pass++)
		for (i = 0; i < nes; i++) {
			e = &es[i];
			if (!(e->flags & SHF_ALLOC) || e->size == 0)
				continue;
			if (strcmp(e->name, ".a5init") == 0)
				k = SEG_A5INIT;
			else if (e->flags & SHF_WRITE || e->type == SHT_NOBITS ||
			    !(e->flags & SHF_EXEC))
				k = SEG_DATA;	/* constants too: data may point at them */
			else
				k = SEG_MAIN;
			/* initialised data, then .bss, so the image stays short */
			if ((pass == 0) != (k != SEG_DATA) ||
			    (pass == 1 && e->type == SHT_NOBITS) ||
			    (pass == 2 && (k != SEG_DATA || e->type != SHT_NOBITS)))
				continue;
			b = segbuf(k);
			a = e->align > 2 ? (long)e->align : 2;
			bput(b, (unsigned char *)0, (a - b->n % a) % a);
			e->seg = k;
			e->base = b->n;
			bput(b, e->data, (long)e->size);
		}
	j = datab.n;
	for (i = 0; i < nsy; i++)
		if (sy[i].shndx == SHN_COMMON) {
			a = sy[i].value > 2 ? (long)sy[i].value : 2;
			j = (j + a - 1) / a * a;
			sy[i].cbase = j;
			j += sy[i].size;
		}
	datalen = (j + 3) & ~3L;
}

static void
fixup(sg, at, loc, tseg, toff, below)
	int sg, tseg;
	long at, loc, toff, below;
{
	struct buf *b = segbuf(sg);
	long v;

	if (tseg == SEG_ABS) {
		wr32(b->p + at, (unsigned long)toff);
		return;
	}
	if (tseg == SEG_DATA)
		v = toff - below;
	else if (tseg == sg)
		v = toff;
	else
		v = 0x20 + 8L * jtindex(tseg, toff + FARHDR) + 2;
	wr32(b->p + at, U32(v));
	if (sg == SEG_DATA)
		addrel(&datar, at);
	else if (tseg == sg)
		addrel(&segr[sg], loc);
	else
		addrel(&a5r[sg], loc);
}

static void
relocate(below)
	long below;
{
	int i, sg, tseg, type;
	long j, at, toff, v, pv;
	unsigned char *r;
	struct esec *e, *t;
	struct esym *s;

	for (i = 0; i < nes; i++) {
		e = &es[i];
		if (e->type != SHT_RELA || e->info >= (unsigned long)nes)
			continue;
		t = &es[e->info];
		if ((sg = t->seg) == 0)
			continue;
		for (j = 0; j + 12 <= (long)e->size; j += 12) {
			r = e->data + j;
			type = rd32(r + 4) & 0xff;
			if ((rd32(r + 4) >> 8) >= (unsigned long)nsy || rd32(r) > t->size - 4 ||
			    t->size < 4)
				die("relocation out of range in %s", t->name);
			s = &sy[rd32(r + 4) >> 8];
			at = t->base + rd32(r);
			if (type == R_68K_NONE)
				continue;
			tseg = target(s, S32(rd32(r + 8)), &toff);
			switch (type) {
			case R_68K_32:
				fixup(sg, at, at + FARHDR, tseg, toff, below);
				break;
			case R_68K_PC32:
			case R_68K_PC16:
			case R_68K_PC8:
				if (tseg != sg)
					die("PC-relative reference to %s across segments", s->name);
				pv = toff - at;
				if (type == R_68K_PC32)
					wr32(segbuf(sg)->p + at, U32(pv));
				else if (type == R_68K_PC16) {
					if (pv < -32768 || pv > 32767)
						die("PC16 reference to %s out of range", s->name);
					wr16(segbuf(sg)->p + at, (unsigned)pv & 0xffff);
				} else {
					if (pv < -128 || pv > 127)
						die("PC8 reference to %s out of range", s->name);
					segbuf(sg)->p[at] = pv;
				}
				break;
			case R_68K_16:
			case R_68K_8:
				if (tseg != SEG_ABS)
					die("16/8-bit absolute reference to %s", s->name);
				v = toff;
				if (type == R_68K_16)
					wr16(segbuf(sg)->p + at, (unsigned)v & 0xffff);
				else
					segbuf(sg)->p[at] = v;
				break;
			default:
				die("unsupported relocation type in %s", t->name);
			}
		}
	}
}

static void
parsespec(spec, named, f)
	char *spec;
	int named;
	struct fset *f;
{
	char *s = xalloc((long)strlen(spec) + 1), *t;
	int k = 0;

	strcpy(s, spec);
	memset(f, 0, sizeof *f);
	f->named = named;
	f->fn = (char **)xalloc((long)(strlen(spec) + 1) * (long)sizeof(char *));
	for (t = strtok(s, ","); t; t = strtok((char *)0, ","), k++) {
		if (k == 0)
			f->id = t;
		else if (k == 1)
			f->ver = strtoul(t, (char **)0, 0);
		else if (k == 2)
			f->minver = strtoul(t, (char **)0, 0);
		else
			f->fn[f->nf++] = t;
	}
	if (k < 4)
		die("export spec needs id,ver,minver,function...: %s", spec);
}

static void
putres(fk, type, id, attr, name, b)
	struct fork *fk;
	char *type, *name;
	int id, attr;
	struct buf *b;
{
	struct res *r = &fk->r[fk->n];
	int i;

	memcpy(r->type, type, 4);
	r->id = id;
	r->attr = attr;
	r->data = b->p;
	r->len = b->n;
	r->doff = fk->n;
	r->noff = 0xffff;
	if (name) {
		r->name = (unsigned char *)xalloc((long)strlen(name) + 1);
		r->name[0] = strlen(name);
		memcpy(r->name + 1, name, strlen(name));
		r->noff = fk->n;
	}
	for (i = 0; i < fk->n; i++)
		if (memcmp(fk->r[i].type, type, 4) == 0)
			break;
	r->tord = i < fk->n ? fk->r[i].tord : (fk->n ? fk->r[fk->n - 1].tord + 1 : 0);
	fk->n++;
}

static int
build(obj, out, lname, lver, code, lid, f1, f2, fs, nfs, creator, fmt)
	char *obj, *out, *lname, *code, *creator, *fmt;
	unsigned lver, f1, f2;
	int lid, nfs;
	struct fset *fs;
{
	struct esym *ent, *m0, *bl;
	long below, off, j, sz;
	int i, k, sg, first[3], cnt[3];
	struct buf jt, sb[3], lr, blk, rf;
	struct libr l;
	struct fork fk;
	unsigned char e[8];

	readelf(obj);
	memset(&jt, 0, sizeof jt);
	memset(sb, 0, sizeof sb);
	memset(&lr, 0, sizeof lr);
	memset(&blk, 0, sizeof blk);
	memset(&rf, 0, sizeof rf);
	layout(fs, nfs);
	below = datalen;
	if ((ent = symbol("__aslm_entry")) == 0 || (m0 = symbol("__aslm_main0")) == 0 ||
	    (bl = symbol("__aslm_blk")) == 0)
		die("the runtime (__aslm_entry, __aslm_main0, __aslm_blk) is not linked in", "");
	if (target(ent, 0L, &off) != SEG_A5INIT || target(bl, 0L, &j) != SEG_A5INIT ||
	    j != segb[SEG_A5INIT].n)
		die("__aslm_blk must end the .a5init section", "");
	if (target(m0, 0L, &off) != SEG_MAIN)
		die("__aslm_main0 must be in Main", "");
	/* fixed entries first: the entry point, then Main's first function */
	(void)target(ent, 0L, &off);
	jtindex(SEG_A5INIT, off + FARHDR);
	(void)target(m0, 0L, &off);
	jtindex(SEG_MAIN, off + FARHDR);
	relocate(below);
	/* exported functions and the descriptors */
	for (i = 0; i < nfs; i++) {
		for (k = 0; k < fs[i].nf; k++) {
			struct esym *s = symbol(fs[i].fn[k]);

			if (s == 0)
				die("exported function %s not defined", fs[i].fn[k]);
			if ((sg = target(s, 0L, &off)) != SEG_MAIN && sg != SEG_A5INIT)
				die("exported %s is not code", fs[i].fn[k]);
			fixup(SEG_DATA, fs[i].desc + 4 + 4L * k, 0L, sg, off, below);
			if (fs[i].named) {
				fixup(SEG_DATA, fs[i].ntab + 4L * k, 0L, SEG_DATA,
				    fs[i].noff[k], below);
			}
		}
		if (fs[i].named)
			fixup(SEG_DATA, fs[i].desc, 0L, SEG_DATA, fs[i].ntab, below);
	}
	/* order the table: segment 1's entries, then segment 2's */
	{
		int ord[1024], n = 0, map[1024];

		for (sg = 1; sg <= 2; sg++) {
			first[sg] = n + 2;
			for (i = 0; i < njt; i++)
				if (jtseg[i] == sg)
					ord[n++] = i;
			cnt[sg] = n + 2 - first[sg];
		}
		for (i = 0; i < n; i++)
			map[ord[i]] = i + 2;
		/* rewrite the provisional indices (request order) */
		for (sg = 1; sg <= 3; sg++) {
			struct rel *r = sg == SEG_DATA ? &datar : &a5r[sg];
			struct buf *b = segbuf(sg);

			for (j = 0; j < r->n; j++) {
				long at = sg == SEG_DATA ? r->off[j] : r->off[j] - FARHDR;
				long v = S32(rd32(b->p + at));

				if (v >= 0x22 && (v - 0x22) % 8 == 0 && (v - 0x22) / 8 < njt)
					wr32(b->p + at, U32(0x20 + 8L * map[(v - 0x22) / 8] + 2));
			}
		}
		b32(&jt, 0x20 + 8L * (n + 2));
		b32(&jt, (unsigned long)below);
		b32(&jt, 8L * (n + 2));
		b32(&jt, 0x20L);
		bput(&jt, (unsigned char *)"\0\0\077\074\0\003\251\360", 8L);
		bput(&jt, (unsigned char *)"\0\0\377\377\0\0\0\0", 8L);
		for (i = 0; i < n; i++) {
			wr16(e, (unsigned)jtseg[ord[i]]);
			wr16(e + 2, 0xa9f0);
			wr32(e + 4, (unsigned long)jtoff[ord[i]]);
			bput(&jt, e, 8L);
		}
	}
	/* the data block that ends %A5Init */
	qsort((char *)datar.off, (size_t)datar.n, sizeof(long), cmplong);
	sz = datab.n;
	while (sz > 0 && datab.p[sz - 1] == 0)
		sz--;
	sz = (sz + 1) & ~1L;
	b32(&blk, BLKMAGIC);
	b32(&blk, (unsigned long)below);
	b32(&blk, (unsigned long)sz);
	b32(&blk, (unsigned long)datar.n);
	b32(&blk, (unsigned long)nfs);
	for (i = 0; i < nfs; i++) {
		b32(&blk, (unsigned long)fs[i].rec);
		b32(&blk, (unsigned long)fs[i].desc);
	}
	for (j = 0; j < datar.n; j++)
		b32(&blk, (unsigned long)datar.off[j]);
	bput(&blk, datab.p, sz);
	bput(&segb[SEG_A5INIT], blk.p, blk.n);
	/* segments */
	for (sg = 1; sg <= 2; sg++) {
		struct seg s;

		memset(&s, 0, sizeof s);
		s.far = 1;
		s.h[H_NEAROFF] = 8L * first[sg];
		s.h[H_NEARCNT] = cnt[sg];
		s.code = segb[sg].p;
		s.clen = segb[sg].n;
		qsort((char *)a5r[sg].off, (size_t)a5r[sg].n, sizeof(long), cmplong);
		qsort((char *)segr[sg].off, (size_t)segr[sg].n, sizeof(long), cmplong);
		s.a5 = a5r[sg];
		s.sr = segr[sg];
		s.gap = (unsigned char *)"";
		if (emitseg(&s, &sb[sg]) < 0)
			die("relocations too far apart to encode", "");
		beven(&sb[sg]);
	}
	/* descriptor */
	memset(&l, 0, sizeof l);
	l.name = lname;
	memcpy(l.code, code, 4);
	l.fmt = 0x0110;
	l.ver = lver;
	l.x = 0x80000001UL;
	l.f1 = f1;
	l.f2 = f2;
	l.nc = nfs;
	l.c = (struct lclass *)xalloc((nfs + 1) * (long)sizeof(struct lclass));
	for (i = 0; i < nfs; i++) {
		l.c[i].kind = 4;
		l.c[i].ver = fs[i].ver;
		l.c[i].minver = fs[i].minver;
		l.c[i].name = fs[i].id;
	}
	emitlibr(&l, &lr);
	/* fork */
	memset(&fk, 0, sizeof fk);
	fk.r = (struct res *)xalloc(8L * (long)sizeof(struct res));
	fk.dataoff = 256;
	putres(&fk, "libr", lid, 0, (char *)0, &lr);
	putres(&fk, code, 0, 0x20, (char *)0, &jt);
	putres(&fk, code, 1, 0x10, "%A5Init", &sb[1]);
	putres(&fk, code, 2, 0x20, "Main", &sb[2]);
	buildfork(&fk, &rf);
	{
		char *base = strrchr(out, '/');

		base = base ? base + 1 : out;
		savefile(out, fmt, base, "libr", creator, &rf);
	}
	printf("%s: %s, %d sets, Main %ld, %%A5Init %ld, A5 world -%#lx..+%#lx\n",
	    out, lname, nfs, segb[2].n, segb[1].n, below, 0x20 + 8L * (njt + 2));
	return 0;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *mode = 0, *in = 0, *only = 0, *out = 0, *lname = "AUXLib$lib", *code = "cdAU";
	char *creator = "AUXn", *fmt = "bin";
	unsigned lver = 0x0110, f1 = 0x0840, f2 = 0x001c;
	int lid = 128, nfs = 0, i;
	struct fset fs[8];
	struct file f;
	struct fork fk;

	for (i = 1; i < argc; i++) {
		char *a = argv[i];

		if (a[0] != '-' || a[1] == 0 || a[2] != 0) {
			if (in)
				only = a;
			else
				in = a;
			continue;
		}
		if (strchr("dtlx", a[1])) {
			mode = a + 1;
			continue;
		}
		if (i + 1 >= argc)
			die("%s needs an argument", a);
		switch (a[1]) {
		case 'o': out = argv[++i]; break;
		case 'n': lname = argv[++i]; break;
		case 'v': lver = strtoul(argv[++i], (char **)0, 0); break;
		case 'c': code = argv[++i]; break;
		case 'i': lid = atoi(argv[++i]); break;
		case 'F': f1 = strtoul(argv[++i], (char **)0, 0); break;
		case 'G': f2 = strtoul(argv[++i], (char **)0, 0); break;
		case 'C': creator = argv[++i]; break;
		case 'f': fmt = argv[++i]; break;
		case 'e':
		case 'u':
			if (nfs == 8)
				die("at most 8 exported sets", "");
			parsespec(argv[++i], a[1] == 'e', &fs[nfs++]);
			break;
		default:
			die("unknown option %s", a);
		}
	}
	if (in == 0)
		die("usage: mkaslm -d|-t|-l|-x file, or mkaslm [options] -o out obj", "");
	if (out) {
		if (strlen(code) != 4 || strlen(creator) != 4)
			die("type and creator codes are 4 characters", "");
		return build(in, out, lname, lver, code, lid, f1, f2, fs, nfs, creator, fmt);
	}
	if (mode == 0)
		die("no mode given", "");
	loadfile(in, &f);
	parsefork(f.rf, f.rlen, &fk);
	switch (*mode) {
	case 'd': return describe(&fk);
	case 't': return roundtrip(&fk, f.rf, f.rlen);
	case 'l': return loadcheck(&fk);
	case 'x': return runlibs(&fk, only);
	}
	return 2;
}
