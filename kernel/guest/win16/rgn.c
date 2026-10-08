/*
 * rgn.c -- regions as lists of disjoint rectangles: visible regions,
 * update regions and the program's clip and GDI regions.  Small lists
 * are the rule (a few windows overlapping), so plain pairwise
 * operations do.
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"

void
r_set(r, l, t, rr, b)
	struct rect *r;
	int l, t, rr, b;
{
	r->l = l;
	r->t = t;
	r->r = rr;
	r->b = b;
}

int
r_and(d, a, b)
	struct rect *d, *a, *b;
{
	struct rect t;

	t.l = a->l > b->l ? a->l : b->l;
	t.t = a->t > b->t ? a->t : b->t;
	t.r = a->r < b->r ? a->r : b->r;
	t.b = a->b < b->b ? a->b : b->b;
	if (R_EMPTY(&t)) {
		r_set(d, 0, 0, 0, 0);
		return 0;
	}
	*d = t;
	return 1;
}

void
r_get(r, a)
	struct rect *r;
	u32 a;
{
	r->l = (short)GW(a);
	r->t = (short)GW(a + 2);
	r->r = (short)GW(a + 4);
	r->b = (short)GW(a + 6);
}

void
r_put(a, r)
	u32 a;
	struct rect *r;
{
	PW(a, r->l);
	PW(a + 2, r->t);
	PW(a + 4, r->r);
	PW(a + 6, r->b);
}

void
rgn_init(g)
	struct rgn *g;
{
	g->n = 0;
	g->max = 0;
	g->r = 0;
	r_set(&g->box, 0, 0, 0, 0);
}

void
rgn_free(g)
	struct rgn *g;
{
	if (g->r)
		free(g->r);
	rgn_init(g);
}

static void
add(g, r)
	struct rgn *g;
	struct rect *r;
{
	if (R_EMPTY(r))
		return;
	if (g->n == g->max) {
		g->max = g->max ? g->max * 2 : 8;
		g->r = (struct rect *)realloc(g->r, g->max * sizeof *g->r);
	}
	g->r[g->n++] = *r;
}

static int
cmp(a, b)
	const void *a, *b;
{
	const struct rect *x = a, *y = b;

	return x->t != y->t ? x->t - y->t : x->l - y->l;
}

/* sort, join neighbours, set the box */
static void
tidy(g)
	struct rgn *g;
{
	int i, j, joined;

	do {
		joined = 0;
		for (i = 0; i < g->n; i++)
			for (j = i + 1; j < g->n; j++) {
				struct rect *a = &g->r[i], *b = &g->r[j];

				if (a->t == b->t && a->b == b->b && (a->r == b->l || b->r == a->l)) {
					a->l = a->l < b->l ? a->l : b->l;
					a->r = a->r > b->r ? a->r : b->r;
				} else if (a->l == b->l && a->r == b->r && (a->b == b->t || b->b == a->t)) {
					a->t = a->t < b->t ? a->t : b->t;
					a->b = a->b > b->b ? a->b : b->b;
				} else
					continue;
				g->r[j] = g->r[--g->n];
				joined = 1;
				j--;
			}
	} while (joined);
	if (g->n > 1)
		qsort(g->r, g->n, sizeof *g->r, cmp);
	if (g->n == 0) {
		r_set(&g->box, 0, 0, 0, 0);
		return;
	}
	g->box = g->r[0];
	for (i = 1; i < g->n; i++) {
		if (g->r[i].l < g->box.l) g->box.l = g->r[i].l;
		if (g->r[i].t < g->box.t) g->box.t = g->r[i].t;
		if (g->r[i].r > g->box.r) g->box.r = g->r[i].r;
		if (g->r[i].b > g->box.b) g->box.b = g->r[i].b;
	}
}

void
rgn_set(g, r)
	struct rgn *g;
	struct rect *r;
{
	g->n = 0;
	add(g, r);
	tidy(g);
}

void
rgn_copy(d, s)
	struct rgn *d, *s;
{
	int i;

	if (d == s)
		return;
	d->n = 0;
	for (i = 0; i < s->n; i++)
		add(d, &s->r[i]);
	d->box = s->box;
	if (d->n == 0)
		r_set(&d->box, 0, 0, 0, 0);
}

/* the pieces of a outside b */
static void
cut(out, a, b)
	struct rgn *out;
	struct rect *a, *b;
{
	struct rect r, m;

	if (!r_and(&m, a, b)) {
		add(out, a);
		return;
	}
	r_set(&r, a->l, a->t, a->r, m.t);	add(out, &r);
	r_set(&r, a->l, m.b, a->r, a->b);	add(out, &r);
	r_set(&r, a->l, m.t, m.l, m.b);		add(out, &r);
	r_set(&r, m.r, m.t, a->r, m.b);		add(out, &r);
}

void
rgn_diff(d, a, b)
	struct rgn *d, *a, *b;
{
	struct rgn cur, nxt;
	int i, j;

	rgn_init(&cur);
	rgn_init(&nxt);
	for (i = 0; i < a->n; i++)
		add(&cur, &a->r[i]);
	for (j = 0; j < b->n && cur.n; j++) {
		struct rect bb = b->r[j];

		nxt.n = 0;
		for (i = 0; i < cur.n; i++)
			cut(&nxt, &cur.r[i], &bb);
		rgn_copy(&cur, &nxt);
	}
	rgn_copy(d, &cur);
	tidy(d);
	rgn_free(&cur);
	rgn_free(&nxt);
}

void
rgn_and(d, a, b)
	struct rgn *d, *a, *b;
{
	struct rgn t;
	struct rect m;
	int i, j;

	rgn_init(&t);
	for (i = 0; i < a->n; i++)
		for (j = 0; j < b->n; j++)
			if (r_and(&m, &a->r[i], &b->r[j]))
				add(&t, &m);
	rgn_copy(d, &t);
	tidy(d);
	rgn_free(&t);
}

void
rgn_or(d, a, b)
	struct rgn *d, *a, *b;
{
	struct rgn t;
	int i;

	rgn_init(&t);
	rgn_diff(&t, b, a);
	for (i = 0; i < a->n; i++)
		add(&t, &a->r[i]);
	rgn_copy(d, &t);
	tidy(d);
	rgn_free(&t);
}

void
rgn_xor(d, a, b)
	struct rgn *d, *a, *b;
{
	struct rgn x, y;
	int i;

	rgn_init(&x);
	rgn_init(&y);
	rgn_diff(&x, a, b);
	rgn_diff(&y, b, a);
	for (i = 0; i < y.n; i++)
		add(&x, &y.r[i]);
	rgn_copy(d, &x);
	tidy(d);
	rgn_free(&x);
	rgn_free(&y);
}

void
rgn_andrect(g, r)
	struct rgn *g;
	struct rect *r;
{
	struct rgn t;

	rgn_init(&t);
	add(&t, r);
	rgn_and(g, g, &t);
	rgn_free(&t);
}

void
rgn_subrect(g, r)
	struct rgn *g;
	struct rect *r;
{
	struct rgn t;

	if (R_EMPTY(r) || g->n == 0)
		return;
	rgn_init(&t);
	add(&t, r);
	rgn_diff(g, g, &t);
	rgn_free(&t);
}

void
rgn_addrect(g, r)
	struct rgn *g;
	struct rect *r;
{
	struct rgn t;

	if (R_EMPTY(r))
		return;
	rgn_init(&t);
	add(&t, r);
	rgn_or(g, g, &t);
	rgn_free(&t);
}

void
rgn_offset(g, dx, dy)
	struct rgn *g;
	int dx, dy;
{
	int i;

	for (i = 0; i < g->n; i++) {
		g->r[i].l += dx;
		g->r[i].r += dx;
		g->r[i].t += dy;
		g->r[i].b += dy;
	}
	if (g->n) {
		g->box.l += dx;
		g->box.r += dx;
		g->box.t += dy;
		g->box.b += dy;
	}
}

int
rgn_ptin(g, x, y)
	struct rgn *g;
	int x, y;
{
	int i;

	for (i = 0; i < g->n; i++)
		if (x >= g->r[i].l && x < g->r[i].r && y >= g->r[i].t && y < g->r[i].b)
			return 1;
	return 0;
}

int
rgn_rectin(g, r)
	struct rgn *g;
	struct rect *r;
{
	struct rect m;
	int i;

	for (i = 0; i < g->n; i++)
		if (r_and(&m, &g->r[i], r))
			return 1;
	return 0;
}

int
rgn_equal(a, b)
	struct rgn *a, *b;
{
	struct rgn t;
	int e;

	rgn_init(&t);
	rgn_xor(&t, a, b);
	e = t.n == 0;
	rgn_free(&t);
	return e;
}

int
rgn_kind(g)
	struct rgn *g;
{
	return g->n == 0 ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION;
}
