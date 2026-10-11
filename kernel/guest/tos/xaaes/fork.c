/*
 * fork.c -- Pvfork and Pfork in one address space.
 *
 * Pvfork's child runs in its parent's memory while the parent sleeps,
 * until the child starts a program (Pexec 200) or exits.  Pfork's child
 * gets the parent's blocks at the same addresses: each block is shared
 * by address, the copy of whoever runs is in place and the others' are
 * in their save buffers, swapped as processes switch:
 *
 *	block --- shreg (live: P) --- P's shadow (save: stale)
 *	                          \-- C's shadow (save: C's copy)
 */
#include "xamint.h"

struct shreg {
	char		*addr;
	long		len;
	struct kp	*live;		/* whose copy is in place; 0 nobody's */
	short		users;
};

struct shadow {
	struct shreg	*r;
	char		*save;
	struct shadow	*next;
};

extern long mem_next(struct kp *k, long after, long *len);

static struct shadow *
shadow_of(struct kp *k, struct shreg *r)
{
	struct shadow *s;

	for (s = k->shadows; s; s = s->next)
		if (s->r == r)
			return s;
	return 0;
}

/* k's copies in place before it runs */
void
fork_swapin(struct kp *k)
{
	struct shadow *s, *o;
	struct shreg *r;

	for (s = k->shadows; s; s = s->next) {
		r = s->r;
		if (r->live == k)
			continue;
		if (r->live && (o = shadow_of(r->live, r)) != 0)
			memcpy(o->save, r->addr, r->len);
		memcpy(r->addr, s->save, r->len);
		r->live = k;
	}
}

static void
drop(struct kp *k, struct shadow *s)
{
	struct shadow **sp;

	for (sp = &k->shadows; *sp; sp = &(*sp)->next)
		if (*sp == s) {
			*sp = s->next;
			break;
		}
	if (s->save)
		kfree_any(s->save);
	kfree_any(s);
}

/* k leaves its blocks to the others sharing them */
void
fork_exit(struct kp *k)
{
	struct shadow *s;
	struct shreg *r;
	struct kp *o, *last;

	while ((s = k->shadows) != 0) {
		r = s->r;
		if (r->live == k)
			r->live = 0;
		drop(k, s);
		last = 0;
		for (o = allprocs; o; o = o->all)
			if (o != k && shadow_of(o, r))
				last = o;
		if (last && mem_region(last, (long)r->addr))
			mem_give(r->addr, last);
		if (--r->users > 1 || !last)
			continue;
		/* one user left: its copy in place, the sharing ends */
		s = shadow_of(last, r);
		if (r->live != last)
			memcpy(r->addr, s->save, r->len);
		drop(last, s);
		kfree_any(r);
	}
}

/* a Pvfork child no longer needs its parent's memory: the parent runs on */
void
vfork_release(struct kp *k)
{
	struct kp *p = k->vfparent;

	if (!p)
		return;
	k->vfparent = 0;
	if (k->p.p_mem != &k->mem) {
		k->p.p_mem->links--;
		k->p.p_mem = &k->mem;
	}
	k->vfdone = 1;
	wake(WAIT_Q, (long)&k->vfdone);
}

/* Pfork: shadows for every block of the parent's, the child's saved copies */
static long
share_mem(struct kp *p, struct kp *c)
{
	struct shadow *s, *n;
	struct shreg *r;
	long a = 0, len;

	fork_swapin(p);
	while ((a = mem_next(p, a, &len)) != 0) {
		for (s = p->shadows; s && s->r->addr != (char *)a; s = s->next)
			;
		if (!s) {
			r = kmalloc_o(sizeof *r, 0);
			s = kmalloc_o(sizeof *s, 0);
			if (s)
				s->save = kmalloc_o(len, 0);
			if (!r || !s || !s->save) {
				if (s && s->save)
					kfree_any(s->save);
				kfree_any(s);
				kfree_any(r);
				return ENOMEM;
			}
			r->addr = (char *)a;
			r->len = len;
			r->live = p;
			r->users = 1;
			s->r = r;
			s->next = p->shadows;
			p->shadows = s;
		}
		r = s->r;
		n = kmalloc_o(sizeof *n, 0);
		if (n)
			n->save = kmalloc_o(len, 0);
		if (!n || !n->save) {
			kfree_any(n);
			return ENOMEM;
		}
		n->r = r;
		memcpy(n->save, r->addr, len);
		n->next = c->shadows;
		c->shadows = n;
		r->users++;
	}
	return 0;
}

/*
 * Pvfork (0x113) and Pfork (0x11b) from trap #1: regs are the caller's
 * d1-d7/a0-a6 with its exception frame after them.  The child returns
 * 0 from the same call.
 */
long
xa_fork(long *regs, long n)
{
	struct kp *p = KP(curproc), *c;
	short *frame = (short *)(regs + 14);
	CONTEXT *x;
	long r;

	if ((frame[0] & 0x2000) || p->kthread)
		return EINVAL;
	c = proc_new(p, p->p.name, 0, 0);
	if (!c)
		return ENOMEM;
	memcpy(c->sa.sigact, p->p.p_sigacts->sigact, sizeof c->sa.sigact);
	c->p.p_sigmask = p->p.p_sigmask;
	c->base = p->base;
	c->mem.base = p->base;
	c->dta = p->dta;
	strncpy(c->p.fname, p->p.fname, sizeof c->p.fname - 1);
	x = &c->p.ctxt[CURRENT];
	bzero(x, sizeof *x);
	x->regs[0] = 0;
	memcpy(&x->regs[1], regs, 14 * sizeof(long));
	x->usp = xa_usp();
	x->pc = *(long *)(frame + 1);
	x->sr = frame[0];
	if (n == 0x113) {
		c->p.p_mem = p->p.p_mem;
		p->p.p_mem->links++;
		c->vfparent = p;
		ready(c);
		xlog("Pvfork: pid %d from %d", c->p.pid, p->p.pid);
		while (!c->vfdone)
			sleep(WAIT_Q, (long)&c->vfdone);
		return c->p.pid;
	}
	r = share_mem(p, c);
	if (r) {
		xlog("Pfork: no memory for the child's copies");
		c->zombie = 1;
		c->p.ppid = 0;
		fork_exit(c);
		fd_closeall(c);
		return r;
	}
	xlog("Pfork: pid %d from %d", c->p.pid, p->p.pid);
	ready(c);
	return c->p.pid;
}
