/*
 * mem16.c -- guest memory: the linear space, the LDT, the global heap
 * (one selector per block, consecutive ones 64K apart for a huge block,
 * the handle being the selector) and local heaps inside a segment.
 *
 * The linear space is handed out first fit from a list of free ranges.
 * A block that grows past its room moves; its selectors stay, only
 * their bases change, so a moved block keeps its handle and its far
 * pointers (as in protected-mode Windows).
 *
 * A local heap's bookkeeping is ours, outside guest memory; a moveable
 * local handle is the offset of a two-word cell in the heap holding the
 * block's offset (as Windows does), so *(char **)h works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"

u8 *M;

void
w16_pw(a, v)
	u32 a, v;
{
	M[a] = v;
	M[a + 1] = v >> 8;
}

void
w16_pl(a, v)
	u32 a, v;
{
	M[a] = v;
	M[a + 1] = v >> 8;
	M[a + 2] = v >> 16;
	M[a + 3] = v >> 24;
}
u32 MSIZE;
struct desc *LDT;
struct gblock *gblk;

/* free linear ranges, sorted */
struct range {
	u32	r_base, r_size;
};
static struct range *fr;
static int nfr, maxfr;

#define	LOWMEM	0x20000		/* below: the BIOS data area and such, never a block */

void
mem_init(size)
	u32 size;
{
	MSIZE = size;
	M = (u8 *)calloc(1, size + 16);
	LDT = (struct desc *)calloc(LDTSIZE, sizeof *LDT);
	gblk = (struct gblock *)calloc(LDTSIZE, sizeof *gblk);
	maxfr = 256;
	fr = (struct range *)malloc(maxfr * sizeof *fr);
	if (!M || !LDT || !gblk || !fr)
		w16_fatal("no memory for %u bytes of guest memory", size);
	fr[0].r_base = LOWMEM;
	fr[0].r_size = size - LOWMEM;
	nfr = 1;
	/* index 0 is never handed out; 1.. are free */
}

/* ---- linear space ---- */

static u32
lalloc(n)
	u32 n;
{
	int i;
	u32 a;

	n = (n + 15) & ~15;
	for (i = 0; i < nfr; i++)
		if (fr[i].r_size >= n) {
			a = fr[i].r_base;
			fr[i].r_base += n;
			fr[i].r_size -= n;
			if (fr[i].r_size == 0) {
				memmove(&fr[i], &fr[i + 1], (nfr - i - 1) * sizeof *fr);
				nfr--;
			}
			return a;
		}
	return 0;
}

static void
lfree(a, n)
	u32 a, n;
{
	int i;

	n = (n + 15) & ~15;
	for (i = 0; i < nfr && fr[i].r_base < a; i++)
		;
	/* join the one before, the one after, or both */
	if (i > 0 && fr[i - 1].r_base + fr[i - 1].r_size == a) {
		fr[i - 1].r_size += n;
		if (i < nfr && a + n == fr[i].r_base) {
			fr[i - 1].r_size += fr[i].r_size;
			memmove(&fr[i], &fr[i + 1], (nfr - i - 1) * sizeof *fr);
			nfr--;
		}
		return;
	}
	if (i < nfr && a + n == fr[i].r_base) {
		fr[i].r_base = a;
		fr[i].r_size += n;
		return;
	}
	if (nfr == maxfr) {
		maxfr *= 2;
		fr = (struct range *)realloc(fr, maxfr * sizeof *fr);
	}
	memmove(&fr[i + 1], &fr[i], (nfr - i) * sizeof *fr);
	fr[i].r_base = a;
	fr[i].r_size = n;
	nfr++;
}

/* grow the range at a (room n) to m in place, if the space after is free */
static int
lgrow(a, n, m)
	u32 a, n, m;
{
	int i;
	u32 need;

	n = (n + 15) & ~15;
	m = (m + 15) & ~15;
	if (m <= n)
		return 1;
	need = m - n;
	for (i = 0; i < nfr; i++)
		if (fr[i].r_base == a + n) {
			if (fr[i].r_size < need)
				return 0;
			fr[i].r_base += need;
			fr[i].r_size -= need;
			if (fr[i].r_size == 0) {
				memmove(&fr[i], &fr[i + 1], (nfr - i - 1) * sizeof *fr);
				nfr--;
			}
			return 1;
		}
	return 0;
}

u32
g_free_bytes()
{
	u32 t = 0;
	int i;

	for (i = 0; i < nfr; i++)
		t += fr[i].r_size;
	return t;
}

/* ---- selectors ---- */

static int selhint = 1;

int
sel_alloc(n)
	int n;
{
	int i, j, pass;

	for (pass = 0; pass < 2; pass++) {
		for (i = pass ? 1 : selhint; i + n <= LDTSIZE; i++) {
			for (j = 0; j < n; j++)
				if (LDT[i + j].d_acc || gblk[i + j].gb_used)
					break;
			if (j == n) {
				for (j = 0; j < n; j++) {
					LDT[i + j].d_acc = D_P | D_S | D_W;	/* claimed */
					LDT[i + j].d_base = 0;
					LDT[i + j].d_limit = 0;
					LDT[i + j].d_flags = j ? DF_HUGE : 0;
				}
				selhint = i + n;
				return SEL(i);
			}
			i += j;
		}
	}
	return 0;
}

void
sel_free(sel, n)
	int sel, n;
{
	int i = SELIX(sel);

	while (n-- > 0 && i < LDTSIZE) {
		memset((char *)&LDT[i], 0, sizeof LDT[i]);
		i++;
	}
	if (SELIX(sel) < selhint)
		selhint = SELIX(sel);
}

void
sel_set(sel, base, limit, acc)
	int sel;
	u32 base, limit;
	int acc;
{
	struct desc *d = &LDT[SELIX(sel)];

	d->d_base = base;
	d->d_limit = limit;
	d->d_acc = acc;
}

u32
sel_base(sel)
	int sel;
{
	return LDT[SELIX(sel)].d_base;
}

/* linear address of sel:off, 0 if not mapped */
u32
lin(sel, off)
	u32 sel, off;
{
	struct desc *d;

	sel &= 0xffff;
	if (!(sel & 4) || SELIX(sel) == 0 || SELIX(sel) >= LDTSIZE)
		return 0;
	d = &LDT[SELIX(sel)];
	if (!(d->d_acc & D_P) || off > d->d_limit)
		return 0;
	return d->d_base + off;
}

char *
gptr(p)
	u32 p;
{
	u32 a = lin(FPSEL(p), FPOFF(p));

	return a ? (char *)M + a : (char *)0;
}

u32
gstrlen(p)
	u32 p;
{
	char *s = gptr(p);

	return s ? strlen(s) : 0;
}

/* ---- global heap ---- */

static void
setsels(ix, base, size, code)
	int ix;
	u32 base, size;
	int code;
{
	struct gblock *b = &gblk[ix];
	int i, acc = D_P | D_S | (code ? D_CODE | D_R : D_W) | D_A;
	u32 left = size ? size : 1;

	for (i = 0; i < b->gb_nsel; i++) {
		LDT[ix + i].d_base = base + (u32)i * 0x10000;
		/* every selector of a huge block reaches to the block's end, at most 64K */
		LDT[ix + i].d_limit = (left > 0x10000 ? 0x10000 : left) - 1;
		LDT[ix + i].d_acc = acc;
		LDT[ix + i].d_flags = i ? DF_HUGE : 0;
		LDT[ix + i].d_owner = b->gb_owner;
		left = left > 0x10000 ? left - 0x10000 : 1;
	}
}

u16
g_alloc(flags, size, owner)
	int flags;
	u32 size;
	int owner;
{
	int nsel = size > 0x10000 ? (size + 0xffff) >> 16 : 1, sel, ix;
	u32 room = (size + 15) & ~15, base;
	struct gblock *b;

	if (size > 16 * 1024 * 1024)
		return 0;
	if (room == 0)
		room = 16;
	if ((sel = sel_alloc(nsel)) == 0)
		return 0;
	if ((base = lalloc(room)) == 0) {
		sel_free(sel, nsel);
		return 0;
	}
	ix = SELIX(sel);
	b = &gblk[ix];
	b->gb_base = base;
	b->gb_size = size;
	b->gb_room = room;
	b->gb_nsel = nsel;
	b->gb_flags = flags;
	b->gb_lock = 0;
	b->gb_owner = owner;
	b->gb_code = 0;
	b->gb_used = 1;
	setsels(ix, base, size, 0);
	if (flags & GMEM_ZEROINIT)
		memset(M + base, 0, room);
	return sel;
}

struct gblock *
g_block(h)
	u32 h;
{
	int ix = SELIX(h & 0xffff);

	if (!(h & 4) || ix == 0 || ix >= LDTSIZE)
		return 0;
	/* a selector in the middle of a huge block belongs to its first */
	while (ix > 0 && (LDT[ix].d_flags & DF_HUGE))
		ix--;
	return gblk[ix].gb_used ? &gblk[ix] : 0;
}

u32
g_size(h)
	u32 h;
{
	struct gblock *b = g_block(h);

	return b ? b->gb_size : 0;
}

int
g_free(h)
	u32 h;
{
	struct gblock *b = g_block(h);
	int ix;

	if (!b)
		return h;
	ix = b - gblk;
	{
		extern void ico_forget();

		ico_forget(SEL(ix));	/* an icon made of it */
	}
	if (b->gb_room)
		lfree(b->gb_base, b->gb_room);
	sel_free(SEL(ix), b->gb_nsel);
	memset((char *)b, 0, sizeof *b);
	return 0;
}

/* the CPU's segment registers on a block that moved or changed size */
static void
segmoved(ix, n)
	int ix, n;
{
	int i, j;

	if (!cpu)
		return;
	for (i = 0; i < 6; i++) {
		j = SELIX(cpu->s[i].sel);
		if ((cpu->s[i].sel & 4) && j >= ix && j < ix + n)
			x86_loadseg(cpu, i, cpu->s[i].sel);
	}
}

u16
g_realloc(h, size, flags)
	u32 h, size;
	int flags;
{
	struct gblock *b = g_block(h);
	int ix, nsel, i;
	u32 room, base, old;

	if (!b)
		return 0;
	ix = b - gblk;
	if (flags & GMEM_MODIFY) {
		b->gb_flags = (b->gb_flags & ~(GMEM_MOVEABLE | GMEM_DISCARDABLE)) |
		    (flags & (GMEM_MOVEABLE | GMEM_DISCARDABLE));
		return SEL(ix);
	}
	/* discarding: the memory goes, the handle stays; GlobalLock gives 0 */
	if (size == 0 && (flags & GMEM_MOVEABLE)) {
		if (b->gb_lock && !b->gb_discarded)
			return 0;
		if (b->gb_room)
			lfree(b->gb_base, b->gb_room);
		if (b->gb_nsel > 1)
			sel_free(SEL(ix + 1), b->gb_nsel - 1);
		b->gb_nsel = 1;
		b->gb_base = b->gb_size = b->gb_room = 0;
		b->gb_discarded = 1;
		setsels(ix, 0, 0, b->gb_code);
		LDT[ix].d_acc &= ~D_P;
		segmoved(ix, 1);
		return SEL(ix);
	}
	if (size == 0)
		size = 1;
	if (b->gb_discarded) {
		/* back from discarded: new memory */
		room = (size + 15) & ~15;
		nsel = size > 0x10000 ? (size + 0xffff) >> 16 : 1;
		for (i = 1; i < nsel; i++)
			if (ix + i >= LDTSIZE || LDT[ix + i].d_acc || gblk[ix + i].gb_used)
				return 0;
		if ((base = lalloc(room)) == 0)
			return 0;
		if (flags & GMEM_ZEROINIT)
			memset(M + base, 0, room);
		b->gb_base = base;
		b->gb_room = room;
		b->gb_size = size;
		b->gb_nsel = nsel;
		b->gb_discarded = 0;
		setsels(ix, base, size, b->gb_code);
		segmoved(ix, nsel);
		return SEL(ix);
	}
	nsel = size > 0x10000 ? (size + 0xffff) >> 16 : 1;
	if (nsel > b->gb_nsel) {
		/* more selectors: the ones after must be free */
		for (i = b->gb_nsel; i < nsel; i++)
			if (ix + i >= LDTSIZE || LDT[ix + i].d_acc || gblk[ix + i].gb_used)
				return 0;
	} else if (nsel < b->gb_nsel)
		sel_free(SEL(ix + nsel), b->gb_nsel - nsel);
	room = (size + 15) & ~15;
	old = b->gb_size;
	if (room > b->gb_room) {
		if (lgrow(b->gb_base, b->gb_room, room))
			b->gb_room = room;
		else {
			if ((base = lalloc(room)) == 0)
				return 0;
			memcpy(M + base, M + b->gb_base, old < room ? old : room);
			lfree(b->gb_base, b->gb_room);
			b->gb_base = base;
			b->gb_room = room;
		}
	}
	if (size > old && (flags & GMEM_ZEROINIT))
		memset(M + b->gb_base + old, 0, size - old);
	b->gb_size = size;
	b->gb_nsel = nsel;
	setsels(ix, b->gb_base, size, b->gb_code);
	segmoved(ix, nsel);
	return SEL(ix);
}

void
g_freeowner(owner)
	int owner;
{
	int i;

	for (i = 1; i < LDTSIZE; i++)
		if (gblk[i].gb_used && gblk[i].gb_owner == owner)
			g_free(SEL(i));
}

/* a data or code selector on the memory of sel */
u16
sel_alias(sel, code)
	int sel, code;
{
	int n = sel_alloc(1);
	struct desc *d = &LDT[SELIX(sel)];

	if (!n)
		return 0;
	sel_set(n, d->d_base, d->d_limit, D_P | D_S | (code ? D_CODE | D_R : D_W) | D_A);
	return n;
}

/* ---- local heaps ---- */

#define	LMAX	1024		/* blocks in one heap */

struct lblk {
	u16	lb_off;		/* data */
	u16	lb_size;	/* room */
	u16	lb_want;	/* bytes asked for */
	u16	lb_cell;	/* moveable: its handle cell, else 0 */
	u8	lb_lock;
	u8	lb_used;
	u16	lb_flags;
};

struct lheap {
	u16	lh_sel;
	u16	lh_start, lh_end;
	int	lh_n;
	struct lblk lh_b[LMAX];	/* sorted by offset; free ones too */
	struct lheap *lh_next;
};

static struct lheap *lheaps;

static struct lheap *
lheap(sel)
	int sel;
{
	struct lheap *h;
	struct gblock *b = g_block(sel);

	if (b)
		sel = SEL(b - gblk);
	for (h = lheaps; h; h = h->lh_next)
		if (h->lh_sel == (sel & 0xffff))
			return h;
	return 0;
}

int
l_init(sel, start, end)
	int sel;
	u32 start, end;
{
	struct lheap *h;
	struct gblock *b = g_block(sel);
	u32 limit;

	if (!b)
		return 0;
	sel = SEL(b - gblk);
	limit = LDT[SELIX(sel)].d_limit;
	if (end > limit)
		end = limit;
	start = (start + 3) & ~3;
	if (start >= end || end - start < 16)
		return 0;
	if ((h = lheap(sel)) == 0) {
		h = (struct lheap *)calloc(1, sizeof *h);
		h->lh_next = lheaps;
		lheaps = h;
	}
	h->lh_sel = sel;
	h->lh_start = start;
	h->lh_end = end;
	h->lh_n = 1;
	h->lh_b[0].lb_off = start;
	h->lh_b[0].lb_size = end - start;
	h->lh_b[0].lb_used = 0;
	/* the instance data's pLocalHeap */
	PW(sel_base(sel) + 6, start);
	return 1;
}

/* grow the segment so the heap can end at newend */
static int
lgrowseg(h, newend)
	struct lheap *h;
	u32 newend;
{
	struct lblk *l;
	u32 add;

	if (newend > 0xfff0)
		newend = 0xfff0;
	if (newend <= h->lh_end)
		return 0;
	if (!g_realloc(h->lh_sel, newend + 16, 0))
		return 0;
	add = newend - h->lh_end;
	l = &h->lh_b[h->lh_n - 1];
	if (!l->lb_used && l->lb_off + l->lb_size == h->lh_end)
		l->lb_size += add;
	else {
		if (h->lh_n == LMAX)
			return 0;
		l = &h->lh_b[h->lh_n++];
		l->lb_off = h->lh_end;
		l->lb_size = add;
		l->lb_used = 0;
		l->lb_cell = 0;
	}
	h->lh_end = newend;
	return 1;
}

/* a free block of n bytes, split off; index or -1 */
static int
lfind(h, n)
	struct lheap *h;
	u32 n;
{
	int i;
	struct lblk *l;

	n = (n + 3) & ~3;
	if (n == 0)
		n = 4;
	for (i = 0; i < h->lh_n; i++) {
		l = &h->lh_b[i];
		if (l->lb_used || l->lb_size < n)
			continue;
		if (l->lb_size - n >= 8 && h->lh_n < LMAX) {
			memmove(&h->lh_b[i + 1], &h->lh_b[i], (h->lh_n - i) * sizeof *l);
			h->lh_n++;
			h->lh_b[i + 1].lb_off = l->lb_off + n;
			h->lh_b[i + 1].lb_size = l->lb_size - n;
			l->lb_size = n;
		}
		l->lb_used = 1;
		l->lb_lock = 0;
		l->lb_cell = 0;
		return i;
	}
	return -1;
}

static int
lget(h, n)
	struct lheap *h;
	u32 n;
{
	int i = lfind(h, n);

	if (i < 0 && lgrowseg(h, h->lh_end + ((n + 0x40f) & ~0x3ff)))
		i = lfind(h, n);
	return i;
}

static void
lrelease(h, i)
	struct lheap *h;
	int i;
{
	struct lblk *l = &h->lh_b[i];

	l->lb_used = 0;
	l->lb_cell = 0;
	if (i + 1 < h->lh_n && !h->lh_b[i + 1].lb_used) {
		l->lb_size += h->lh_b[i + 1].lb_size;
		memmove(&h->lh_b[i + 1], &h->lh_b[i + 2], (h->lh_n - i - 2) * sizeof *l);
		h->lh_n--;
	}
	if (i > 0 && !h->lh_b[i - 1].lb_used) {
		h->lh_b[i - 1].lb_size += l->lb_size;
		memmove(&h->lh_b[i], &h->lh_b[i + 1], (h->lh_n - i - 1) * sizeof *l);
		h->lh_n--;
	}
}

/* the block a handle names: index, -1 none */
static int
lbyhandle(h, hd)
	struct lheap *h;
	u32 hd;
{
	int i;

	for (i = 0; i < h->lh_n; i++)
		if (h->lh_b[i].lb_used && (h->lh_b[i].lb_cell ? h->lh_b[i].lb_cell == hd :
		    h->lh_b[i].lb_off == hd))
			return i;
	return -1;
}

u16
l_alloc(sel, flags, size)
	int sel, flags;
	u32 size;
{
	struct lheap *h = lheap(sel);
	int i, c;
	u32 base;
	u16 off, cell = 0;

	if (!h || size > 0xfff0)
		return 0;
	if (flags & LMEM_MOVEABLE) {
		if ((c = lget(h, 4)) < 0)
			return 0;
		h->lh_b[c].lb_flags = 0x8000;	/* a cell */
		cell = h->lh_b[c].lb_off;
	}
	if ((i = lget(h, size)) < 0) {
		if (cell)
			lrelease(h, lbyhandle(h, cell));
		return 0;
	}
	base = sel_base(h->lh_sel);
	off = h->lh_b[i].lb_off;
	h->lh_b[i].lb_want = size;
	h->lh_b[i].lb_flags = flags;
	if (flags & LMEM_ZEROINIT)
		memset(M + base + off, 0, h->lh_b[i].lb_size);
	if (cell) {
		h->lh_b[i].lb_cell = cell;
		PW(base + cell, off);
		PW(base + cell + 2, 0);
		return cell;
	}
	return off;
}

u16
l_free(sel, hd)
	int sel;
	u32 hd;
{
	struct lheap *h = lheap(sel);
	int i;
	u16 cell;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return hd;
	cell = h->lh_b[i].lb_cell;
	lrelease(h, i);
	if (cell) {
		for (i = 0; i < h->lh_n; i++)
			if (h->lh_b[i].lb_used && h->lh_b[i].lb_off == cell && h->lh_b[i].lb_flags == 0x8000) {
				lrelease(h, i);
				break;
			}
	}
	return 0;
}

u16
l_realloc(sel, hd, size, flags)
	int sel, flags;
	u32 hd, size;
{
	struct lheap *h = lheap(sel);
	int i, j;
	u32 base;
	u16 off, cell, want;
	struct lblk *l;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return 0;
	if (flags & LMEM_MODIFY)
		return hd;
	if (size == 0)
		size = 1;
	l = &h->lh_b[i];
	base = sel_base(h->lh_sel);
	if (((size + 3) & ~3) <= l->lb_size) {
		l->lb_want = size;
		return hd;
	}
	/* grow into a free block after */
	if (i + 1 < h->lh_n && !h->lh_b[i + 1].lb_used &&
	    l->lb_size + h->lh_b[i + 1].lb_size >= ((size + 3) & ~3)) {
		u32 need = ((size + 3) & ~3) - l->lb_size;

		if (h->lh_b[i + 1].lb_size - need >= 8) {
			h->lh_b[i + 1].lb_off += need;
			h->lh_b[i + 1].lb_size -= need;
			l->lb_size += need;
		} else {
			l->lb_size += h->lh_b[i + 1].lb_size;
			memmove(&h->lh_b[i + 1], &h->lh_b[i + 2], (h->lh_n - i - 2) * sizeof *l);
			h->lh_n--;
		}
		if (flags & LMEM_ZEROINIT)
			memset(M + base + l->lb_off + l->lb_want, 0, size - l->lb_want);
		l->lb_want = size;
		return hd;
	}
	/* move: a fixed block only with LMEM_MOVEABLE, as Windows */
	cell = l->lb_cell;
	if (!cell && !(flags & LMEM_MOVEABLE))
		return 0;
	off = l->lb_off;
	want = l->lb_want;
	if ((j = lget(h, size)) < 0)
		return 0;
	base = sel_base(h->lh_sel);	/* the segment may have grown elsewhere */
	i = lbyhandle(h, hd);		/* indices moved */
	l = &h->lh_b[i];
	memmove(M + base + h->lh_b[j].lb_off, M + base + off, want);
	if (flags & LMEM_ZEROINIT)
		memset(M + base + h->lh_b[j].lb_off + want, 0, size - want);
	h->lh_b[j].lb_want = size;
	h->lh_b[j].lb_flags = l->lb_flags;
	h->lh_b[j].lb_cell = cell;
	h->lh_b[j].lb_lock = l->lb_lock;
	off = h->lh_b[j].lb_off;
	l->lb_cell = 0;
	lrelease(h, i);
	if (cell) {
		PW(base + cell, off);
		return cell;
	}
	return off;
}

u16
l_lock(sel, hd)
	int sel;
	u32 hd;
{
	struct lheap *h = lheap(sel);
	int i;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return 0;
	h->lh_b[i].lb_lock++;
	return h->lh_b[i].lb_off;
}

int
l_unlock(sel, hd)
	int sel;
	u32 hd;
{
	struct lheap *h = lheap(sel);
	int i;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return 0;
	if (h->lh_b[i].lb_lock)
		h->lh_b[i].lb_lock--;
	return h->lh_b[i].lb_lock;
}

u16
l_size(sel, hd)
	int sel;
	u32 hd;
{
	struct lheap *h = lheap(sel);
	int i;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return 0;
	return h->lh_b[i].lb_size;
}

u16
l_flags(sel, hd)
	int sel;
	u32 hd;
{
	struct lheap *h = lheap(sel);
	int i;

	if (!h || (i = lbyhandle(h, hd)) < 0)
		return 0;
	return h->lh_b[i].lb_lock;
}

u16
l_handle(sel, off)
	int sel;
	u32 off;
{
	struct lheap *h = lheap(sel);
	int i;

	if (!h)
		return 0;
	for (i = 0; i < h->lh_n; i++)
		if (h->lh_b[i].lb_used && h->lh_b[i].lb_off == off)
			return h->lh_b[i].lb_cell ? h->lh_b[i].lb_cell : off;
	return 0;
}

u16
l_compact(sel, want)
	int sel;
	u32 want;
{
	struct lheap *h = lheap(sel);
	int i;
	u32 big = 0;

	if (!h)
		return 0;
	for (i = 0; i < h->lh_n; i++)
		if (!h->lh_b[i].lb_used && h->lh_b[i].lb_size > big)
			big = h->lh_b[i].lb_size;
	if (big < want && lgrowseg(h, h->lh_end + want))
		return l_compact(sel, 0);
	return big;
}
