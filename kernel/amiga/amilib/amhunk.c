/*
 * amhunk.c -- LoadSeg for a load file already in memory.
 *
 * Each hunk becomes an AmigaOS segment, as LoadSeg makes them: a long
 * holding the allocation size, the BPTR to the next segment, then the
 * hunk's contents.  The result is the BPTR of the first segment.
 *
 * Accepted: HUNK_HEADER with no resident library names, CODE, DATA, BSS,
 * RELOC32, RELOC32SHORT (and the old DREL32 number LoadSeg reads the
 * same way), RELRELOC32, SYMBOL, DEBUG, END; memory-type bits in the
 * header and on hunk types.  Overlays and anything else: AMH_EFORMAT.
 * Every count and offset is checked against the file and the hunk.
 */

#include "amilib.h"

#define	HUNK_CODE		0x3e9
#define	HUNK_DATA		0x3ea
#define	HUNK_BSS		0x3eb
#define	HUNK_RELOC32		0x3ec
#define	HUNK_SYMBOL		0x3f0
#define	HUNK_DEBUG		0x3f1
#define	HUNK_END		0x3f2
#define	HUNK_HEADER		0x3f3
#define	HUNK_DREL32		0x3f7
#define	HUNK_RELOC32SHORT	0x3fc
#define	HUNK_RELRELOC32		0x3fd

#define	AMH_MAXHUNKS		64

struct hk {
	unsigned char	*p, *e;		/* read position, end of file */
	int		bad;
};

static unsigned long
get32(h)
	struct hk *h;
{
	unsigned long v;

	if (h->e - h->p < 4) {
		h->bad = 1;
		return 0;
	}
	v = ((unsigned long)h->p[0] << 24) | ((unsigned long)h->p[1] << 16) |
	    ((unsigned long)h->p[2] << 8) | h->p[3];
	h->p += 4;
	return v;
}

static unsigned long
get16(h)
	struct hk *h;
{
	unsigned long v;

	if (h->e - h->p < 2) {
		h->bad = 1;
		return 0;
	}
	v = ((unsigned long)h->p[0] << 8) | h->p[1];
	h->p += 2;
	return v;
}

static void
skip(h, n)
	struct hk *h;
	unsigned long n;
{
	if ((unsigned long)(h->e - h->p) < n)
		h->bad = 1;
	else
		h->p += n;
}

/* the segment's memory: s points at the "next" long */
#define	SEGDATA(s)	((s) + 4)

void
am_unloadseg(seg)
	unsigned long seg;
{
	char *s;
	unsigned long next;

	while (seg) {
		s = (char *)(seg << 2);
		next = AL(s, 0);
		am_free(s - 4, AL(s, -4));
		seg = next;
	}
}

/* one relocation: the long at off in hunk cur gets base[tgt] added */
static int
reloc(seg, size, n, cur, off, tgt, rel)
	char **seg;
	unsigned long *size;
	int n, cur, tgt, rel;
	unsigned long off;
{
	char *p;
	unsigned long v;

	if (tgt < 0 || tgt >= n || off > size[cur] || size[cur] - off < 4)
		return -1;
	p = SEGDATA(seg[cur]) + off;
	v = ((unsigned long)(unsigned char)p[0] << 24) |
	    ((unsigned long)(unsigned char)p[1] << 16) |
	    ((unsigned long)(unsigned char)p[2] << 8) | (unsigned char)p[3];
	v += (unsigned long)SEGDATA(seg[tgt]);
	if (rel)
		v -= (unsigned long)p;
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
	return 0;
}

unsigned long
am_loadseg(buf, len, errp)
	char *buf;
	unsigned long len;
	int *errp;
{
	struct hk h;
	char *seg[AMH_MAXHUNKS];
	unsigned long size[AMH_MAXHUNKS], v, t, n, cnt, tgt, off, i, k;
	unsigned long first, last;
	int nh, cur = -1, e = AMH_EFORMAT, shortrel;

	h.p = (unsigned char *)buf;
	h.e = h.p + len;
	h.bad = 0;
	for (i = 0; i < AMH_MAXHUNKS; i++)
		seg[i] = 0;
	if (get32(&h) != HUNK_HEADER || get32(&h) != 0)	/* no lib names */
		goto fail;
	n = get32(&h);
	first = get32(&h);
	last = get32(&h);
	if (h.bad || first != 0 || last < first || last - first + 1 != n ||
	    n == 0 || n > AMH_MAXHUNKS)
		goto fail;
	nh = n;
	for (i = 0; i < n; i++) {
		v = get32(&h);
		if ((v >> 30) == 3)
			(void)get32(&h);		/* extended attributes */
		size[i] = (v & 0x3fffffff) << 2;
		if (h.bad || size[i] > 0x1000000)
			goto fail;
	}
	/* allocate every hunk first: relocations may point forward */
	e = AMH_ENOMEM;
	for (i = 0; i < n; i++) {
		seg[i] = am_alloc(size[i] + 8, MEMF_PUBLIC | MEMF_CLEAR);
		if (seg[i] == 0)
			goto fail;
		AL(seg[i], 0) = size[i] + 8;
		seg[i] += 4;
		if (i)
			AL(seg[i - 1], 0) = (unsigned long)seg[i] >> 2;
	}
	e = AMH_EFORMAT;
	while (h.p < h.e) {
		t = get32(&h) & 0x3fffffff;
		switch (t) {
		case HUNK_CODE:
		case HUNK_DATA:
		case HUNK_BSS:
			if (++cur >= nh)
				goto fail;
			v = get32(&h) << 2;
			if (h.bad || v > size[cur])
				goto fail;
			if (t != HUNK_BSS) {
				if ((unsigned long)(h.e - h.p) < v)
					goto fail;
				for (i = 0; i < v; i++)
					SEGDATA(seg[cur])[i] = h.p[i];
				h.p += v;
			}
			break;
		case HUNK_RELOC32:
		case HUNK_RELOC32SHORT:
		case HUNK_DREL32:
		case HUNK_RELRELOC32:
			if (cur < 0)
				goto fail;
			shortrel = t == HUNK_RELOC32SHORT || t == HUNK_DREL32;
			for (k = 0; ; ) {
				cnt = shortrel ? get16(&h) : get32(&h);
				k += shortrel;
				if (h.bad)
					goto fail;
				if (cnt == 0)
					break;
				tgt = shortrel ? get16(&h) : get32(&h);
				k += shortrel;
				if (cnt > len)
					goto fail;
				while (cnt--) {
					off = shortrel ? get16(&h) : get32(&h);
					k += shortrel;
					if (h.bad || reloc(seg, size, nh, cur, off,
					    (int)tgt, t == HUNK_RELRELOC32) != 0)
						goto fail;
				}
			}
			if (k & 1)			/* to a long boundary */
				(void)get16(&h);
			break;
		case HUNK_SYMBOL:
			while ((v = get32(&h)) != 0 && !h.bad)
				skip(&h, (v & 0xffffff) * 4 + 4);
			break;
		case HUNK_DEBUG:
			v = get32(&h);
			skip(&h, v * 4);
			break;
		case HUNK_END:
			break;
		default:
			goto fail;
		}
		if (h.bad)
			goto fail;
	}
	if (cur != nh - 1)
		goto fail;
	amx_cacheflush();
	*errp = 0;
	return (unsigned long)seg[0] >> 2;
fail:
	for (i = 0; i < AMH_MAXHUNKS && seg[i]; i++)
		am_free(seg[i] - 4, AL(seg[i], -4));
	*errp = e;
	return 0;
}
