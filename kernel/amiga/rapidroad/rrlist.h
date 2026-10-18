/*
 * rrlist.h - the doubly linked list the host controller driver keeps its
 * queue heads and transfer descriptors on (the shape of Linux's list.h,
 * written for GCC 2.7.2: no typeof, no statement expressions).
 */
#ifndef RRLIST_H
#define RRLIST_H

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name)	{ &(name), &(name) }
#define INIT_LIST_HEAD(p)	((p)->next = (p)->prev = (p))

/* the structure holding the member m at p */
#define list_entry(p, type, m)	((type *)((char *)(p) - (unsigned long)&((type *)0)->m))

#define list_empty(h)		((h)->next == (h))
#define list_is_last(e, h)	((e)->next == (h))

static void
list_add_tail(e, h)
struct list_head *e, *h;
{
	e->prev = h->prev;
	e->next = h;
	h->prev->next = e;
	h->prev = e;
}

static void
list_del(e)
struct list_head *e;
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	e->next = e->prev = e;
}

/* move the elements of a onto the end of h; a is empty afterwards */
static void
list_splice_tail(a, h)
struct list_head *a, *h;
{
	if (list_empty(a))
		return;
	a->next->prev = h->prev;
	h->prev->next = a->next;
	a->prev->next = h;
	h->prev = a->prev;
	INIT_LIST_HEAD(a);
}

#endif /* RRLIST_H */
