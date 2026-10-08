/*
 * hook.c -- USER's hooks: SetWindowsHook(Ex), the chains they make and
 * the calls into them where Windows makes them (hook_call from user.c,
 * dialog.c, menu.c).  A chain is a hook type's hooks, the newest first;
 * one set with a task is that task's alone, the others the session's.
 * Each hook passes the call on (CallNextHookEx, DefHookProc) or not.
 * A hook is not called again from within itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"

#define	NHOOK	64
#define	HBASE	0x484b0000L	/* handles: "HK" and the slot + 1 */

struct hook {
	int	used;
	int	id;		/* WH_ */
	u32	proc;
	u16	task;		/* its task's handle, 0 the session's */
	u16	hinst;
	int	busy;		/* being called */
	long	order;		/* when set: the newest is called first */
};

static struct hook hooks[NHOOK];
static long norder;
static int nset[WH_MAX - WH_MIN + 1];	/* how many of each type */

static struct hook *
hget(h)
	u32 h;
{
	long i = (long)(h - HBASE) - 1;

	if ((h & 0xffff0000L) != HBASE || i < 0 || i >= NHOOK || !hooks[i].used)
		return 0;
	return &hooks[i];
}

/* the next hook of a chain to call after one (0: the first) */
static struct hook *
next(id, after)
	int id;
	struct hook *after;
{
	struct hook *h, *best = 0;
	u16 t = curtask ? curtask->t_htask : 0;
	int i;

	for (i = 0; i < NHOOK; i++) {
		h = &hooks[i];
		if (!h->used || h->id != id || h->busy || (h->task && h->task != t))
			continue;
		if (after && h->order >= after->order)
			continue;
		if (!best || h->order > best->order)
			best = h;
	}
	return best;
}

static u32
callone(h, code, wp, lp)
	struct hook *h;
	int code;
	u32 wp, lp;
{
	u32 r;

	h->busy = 1;
	cb_begin();
	cb_push16(code);
	cb_push16(wp);
	cb_push32(lp);
	r = cb_call(h->proc, 0);
	/* it may have unhooked itself */
	h->busy = 0;
	return r;
}

/* any hook of the type to call? */
int
hook_any(id)
	int id;
{
	return id >= WH_MIN && id <= WH_MAX && nset[id - WH_MIN] > 0 && next(id, (struct hook *)0) != 0;
}

/* the chain of a type called: what its first hook returns, 0 none */
u32
hook_call(id, code, wp, lp)
	int id, code;
	u32 wp, lp;
{
	struct hook *h;

	if (id < WH_MIN || id > WH_MAX || !nset[id - WH_MIN] || (h = next(id, (struct hook *)0)) == 0)
		return 0;
	return callone(h, code, wp, lp);
}

static u32
set(id, proc, hinst, task)
	int id;
	u32 proc;
	u16 hinst, task;
{
	int i;

	if (id < WH_MIN || id > WH_MAX || !proc)
		return 0;
	for (i = 0; i < NHOOK && hooks[i].used; i++)
		;
	if (i == NHOOK)
		return 0;
	memset((char *)&hooks[i], 0, sizeof hooks[i]);
	hooks[i].used = 1;
	hooks[i].id = id;
	hooks[i].proc = proc;
	hooks[i].hinst = hinst;
	hooks[i].task = task;
	hooks[i].order = ++norder;
	nset[id - WH_MIN]++;
	if (w16_debug)
		w16_log("startwin: hook %d set (%08lx%s)\n", id, (long)proc, task ? ", a task's" : "");
	return HBASE + i + 1;
}

static void
unset(h)
	struct hook *h;
{
	nset[h->id - WH_MIN]--;
	h->used = 0;
}

/* SetWindowsHookEx(id, proc, hInstance, hTask): the hook's handle */
static u32
u_SetWindowsHookEx(a)
	u32 *a;
{
	return set((short)a[0], a[1], (u16)a[2], (u16)a[3]);
}

/* SetWindowsHook(id, proc): the session's but WH_MSGFILTER, the task's; its handle, for DefHookProc */
static u32
u_SetWindowsHook(a)
	u32 *a;
{
	int id = (short)a[0];

	return set(id, a[1], 0, id == WH_MSGFILTER && curtask ? curtask->t_htask : 0);
}

static u32
u_UnhookWindowsHookEx(a)
	u32 *a;
{
	struct hook *h = hget(a[0]);

	if (!h)
		return 0;
	unset(h);
	return 1;
}

/* UnhookWindowsHook(id, proc) */
static u32
u_UnhookWindowsHook(a)
	u32 *a;
{
	struct hook *h, *best = 0;
	int i;

	for (i = 0; i < NHOOK; i++) {
		h = &hooks[i];
		if (h->used && h->id == (short)a[0] && h->proc == a[1] && (!best || h->order > best->order))
			best = h;
	}
	if (!best)
		return 0;
	unset(best);
	return 1;
}

/* CallNextHookEx(hhook, code, wParam, lParam) */
static u32
u_CallNextHookEx(a)
	u32 *a;
{
	struct hook *h = hget(a[0]), *n;

	if (!h || (n = next(h->id, h)) == 0)
		return 0;
	return callone(n, (short)a[1], a[2] & 0xffff, a[3]);
}

/* DefHookProc(code, wParam, lParam, HHOOK far *): the hook after *it */
static u32
u_DefHookProc(a)
	u32 *a;
{
	u32 p = lin(FPSEL(a[3]), FPOFF(a[3])), b[4];

	if (!p)
		return 0;
	b[0] = GL(p);
	b[1] = a[0];
	b[2] = a[1];
	b[3] = a[2];
	return u_CallNextHookEx(b);
}

/* CallMsgFilter(MSG far *, code): the system's message filters, then the task's; nonzero: taken */
u32
hook_msgfilter(msg, code)
	u32 msg;
	int code;
{
	if (hook_call(WH_SYSMSGFILTER, code, 0, msg))
		return 1;
	return hook_call(WH_MSGFILTER, code, 0, msg) ? 1 : 0;
}

static u32
u_CallMsgFilter(a)
	u32 *a;
{
	return hook_msgfilter(a[0], (short)a[1]);
}

/* a task ended: its hooks go, and those its code is in */
void
hook_taskended(t)
	struct task *t;
{
	int i;

	for (i = 0; i < NHOOK; i++)
		if (hooks[i].used && ((hooks[i].task && hooks[i].task == t->t_htask) ||
		    (hooks[i].hinst && hooks[i].hinst == t->t_hinst)))
			unset(&hooks[i]);
}

struct impl hk_impl[] = {
	{ "USER", "SetWindowsHook", u_SetWindowsHook },
	{ "USER", "SetWindowsHookEx", u_SetWindowsHookEx },
	{ "USER", "UnhookWindowsHook", u_UnhookWindowsHook },
	{ "USER", "UnhookWindowsHookEx", u_UnhookWindowsHookEx },
	{ "USER", "CallNextHookEx", u_CallNextHookEx },
	{ "USER", "DefHookProc", u_DefHookProc },
	{ "USER", "CallMsgFilter", u_CallMsgFilter },
	{ 0 }
};
