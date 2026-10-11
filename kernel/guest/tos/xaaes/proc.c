/*
 * proc.c -- processes and kernel threads in one address space, their
 * queues, sleep and wake, timeouts, signals and process extensions.
 *
 * A process switches only in the kernel: when it sleeps, yields, or on
 * its way back to user mode after the tick asked for a switch.  Each
 * has its own kernel stack (MiNT's p->stack), which holds its trap
 * frames; xa_swtch saves the callee-saved registers and USP there.
 * The root is the idle loop: it runs due timeouts, polls the select
 * waiters and stops until the next interrupt when nothing is ready.
 */
#include <stddef.h>
#include "xamint.h"

struct proc *curproc, *rootproc;
struct kp *allprocs;
volatile short xa_resched;
const long xa_sigoff = offsetof(struct proc, sigpending);

static struct proc_queue sysq[NUM_QUEUES];
static short nextpid;
static TIMEOUT *tlist;			/* by absolute time in when */
static short nsel;			/* processes in SELECT_Q */

static void handle_sigs(void);
void reap_orphans(void);

static void
rm_q(int que, struct proc *p)
{
	struct proc_queue *q = &sysq[que];

	if (p->q_prev)
		p->q_prev->q_next = p->q_next;
	else if (q->head == p)
		q->head = p->q_next;
	if (p->q_next)
		p->q_next->q_prev = p->q_prev;
	else if (q->tail == p)
		q->tail = p->q_prev;
	p->q_next = p->q_prev = 0;
	if (que == SELECT_Q)
		nsel--;
}

static void
add_q(int que, struct proc *p)
{
	struct proc_queue *q = &sysq[que];

	p->q_next = 0;
	p->q_prev = q->tail;
	if (q->tail)
		q->tail->q_next = p;
	else
		q->head = p;
	q->tail = p;
	p->wait_q = que;
	if (que == SELECT_Q)
		nsel++;
}

/* to the ready queue from wherever it waits */
void
ready(struct kp *k)
{
	struct proc *p = &k->p;
	short s = xa_spl7();

	if (p->wait_q != READY_Q && p->wait_q != CURPROC_Q && p->wait_q != ZOMBIE_Q) {
		rm_q(p->wait_q, p);
		add_q(READY_Q, p);
		xa_resched = 1;
	}
	xa_splx(s);
}

/* a zeroed process with its own substructures */
static struct kp *
kp_alloc(const char *name)
{
	struct kp *k = kmalloc_o(sizeof *k, 0);

	if (!k)
		return 0;
	bzero(k, sizeof *k);
	k->p.magic = CTXT_MAGIC;
	k->p.p_mem = &k->mem;
	k->p.p_cred = &k->cred;
	k->cred.ucr = &k->ucr;
	k->p.p_fd = &k->fd;
	k->fd.ofiles = &k->ofiles[-MIN_HANDLE];
	k->fd.ofileflags = &k->oflags[-MIN_HANDLE];
	k->fd.nfiles = NOFILE;
	k->p.p_cwd = &k->cwd;
	k->p.p_sigacts = &k->sa;
	k->p.p_ext = &k->ext;
	k->ext.size = 10;
	k->mem.links = k->fd.links = k->cwd.links = k->sa.links = 1;
	k->p.stack_magic = STACK_MAGIC;
	strncpy(k->p.name, name, PNAMSIZ);
	return k;
}

void
proc_init(void)
{
	struct kp *k = kp_alloc("MiNT");

	k->p.domain = DOM_MINT;
	k->cwd.curdrv = 2;
	k->p.wait_q = CURPROC_Q;
	allprocs = k;
	rootproc = curproc = &k->p;
}

static int
pidused(int pid)
{
	struct kp *k;

	for (k = allprocs; k; k = k->all)
		if (k->p.pid == pid)
			return 1;
	return 0;
}

/* a new process or kernel thread, not yet running */
struct kp *
proc_new(struct kp *parent, const char *name, int kthread, int share)
{
	struct kp *k = kp_alloc(name);
	long *sp;
	int i;

	if (!k)
		return 0;
	do
		if (++nextpid > 30000)
			nextpid = 2;
	while (pidused(nextpid));
	k->p.pid = nextpid;
	k->p.ppid = parent ? parent->p.pid : 0;
	k->p.pgrp = parent ? parent->p.pgrp : k->p.pid;
	k->p.domain = parent ? parent->p.domain : DOM_TOS;
	k->kthread = kthread;
	k->p.started.tv_sec = hc(H_TIME, 0);
	if (parent) {
		k->cwd.curdrv = parent->p.p_cwd->curdrv;
		k->cwd.cmask = parent->p.p_cwd->cmask;
		k->p.p_sigmask = parent->p.p_sigmask;
		if (share) {
			k->p.p_fd = parent->p.p_fd;
			k->p.p_cwd = parent->p.p_cwd;
			k->p.p_mem = parent->p.p_mem;
			k->p.p_sigacts = parent->p.p_sigacts;
			KP(k)->base = parent->base;
			parent->p.p_fd->links++;
			parent->p.p_cwd->links++;
			parent->p.p_mem->links++;
			parent->p.p_sigacts->links++;
		} else {
			fd_inherit(k, parent);
			for (i = 0; i < 26; i++)
				if (parent->dirs[i])
					dir_set(k, i, parent->dirs[i]);
		}
		k->dta = parent->dta;
	}
	sp = (long *)((long)(k->p.stack + STKSIZE) & ~3L);
	k->p.sysstack = (long)sp;
	*--sp = (long)xa_kstart;
	for (i = 0; i < 11; i++)
		*--sp = 0;
	*--sp = 0;
	k->ksp = (long)sp;
	k->p.wait_q = STOP_Q;
	add_q(STOP_Q, &k->p);
	k->all = allprocs;
	allprocs = k;
	return k;
}

struct proc *_cdecl
get_curproc(void)
{
	return curproc;
}

struct proc *_cdecl
pid2proc(int pid)
{
	struct kp *k;

	for (k = allprocs; k; k = k->all)
		if (k->p.pid == pid && !k->zombie)
			return &k->p;
	return 0;
}

/* ---- timeouts ---- */

static TIMEOUT *
newtimeout(struct proc *p, long delta, void (*func)(PROC *, long), long arg)
{
	TIMEOUT *t = kmalloc_o(sizeof *t, 0), **tp;
	short s;

	if (xa_tracing)
		xlog("timeout %lx in %ld for %d", (long)func, delta, p->pid);
	if (!t)
		return 0;
	t->proc = p;
	t->when = now_ms() + (delta > 0 ? delta : 0);
	t->func = (to_func *)func;
	t->arg = arg;
	t->flags = 0;
	s = xa_spl7();
	for (tp = &tlist; *tp && (*tp)->when <= t->when; tp = &(*tp)->next)
		;
	t->next = *tp;
	*tp = t;
	xa_splx(s);
	return t;
}

static void
rmtimeout(TIMEOUT *t)
{
	TIMEOUT **tp;
	short s = xa_spl7();

	for (tp = &tlist; *tp; tp = &(*tp)->next)
		if (*tp == t) {
			*tp = t->next;
			xa_splx(s);
			kfree_any(t);
			return;
		}
	xa_splx(s);
}

TIMEOUT *_cdecl
addtimeout_curproc(long delta, void (*func)(PROC *, long))
{
	return newtimeout(curproc, delta, func, 0);
}

TIMEOUT *_cdecl
addroottimeout(long delta, void (*func)(PROC *, long), unsigned short flags)
{
	(void)flags;
	return newtimeout(rootproc, delta, func, 0);
}

void _cdecl
canceltimeout(TIMEOUT *t)
{
	rmtimeout(t);
}

void _cdecl
cancelroottimeout(TIMEOUT *t)
{
	rmtimeout(t);
}

static void
cancelproc(struct proc *p)
{
	TIMEOUT *t;
	short s;

again:
	s = xa_spl7();
	for (t = tlist; t; t = t->next)
		if (t->proc == p) {
			xa_splx(s);
			rmtimeout(t);
			goto again;
		}
	xa_splx(s);
}

/*
 * Due timeouts.  Their functions may sleep, so they run only where a
 * process stands outside the queues: the root's idle loop, or a
 * process on its way back to user mode.
 */
void
run_timeouts(void)
{
	TIMEOUT *t;
	long now = now_ms();
	short s;

	reap_orphans();
	for (;;) {
		s = xa_spl7();
		t = tlist;
		if (!t || t->when > now) {
			xa_splx(s);
			break;
		}
		tlist = t->next;
		xa_splx(s);
		(*t->func)(t->proc, t->arg);
		kfree_any(t);
	}
}

/* naps that are over and select waiters with something to read or write */
static void
poll_waiters(void)
{
	struct kp *k;
	long now = now_ms();

	for (k = allprocs; k; k = k->all) {
		if (k->p.wait_q == READY_Q || k->p.wait_q == CURPROC_Q || k->p.wait_q == ZOMBIE_Q)
			continue;
		if (k->wake_at && k->wake_at <= now)
			ready(k);
		else if (k->p.wait_q == SELECT_Q && (k->sel_r || k->sel_w)) {
			long r = k->sel_r ? *k->sel_r : 0, w = k->sel_w ? *k->sel_w : 0;
			int i;

			for (i = 0; i < 32; i++)
				if (((r >> i) & 1 && file_select(fd_get(k, i), 0)) ||
				    ((w >> i) & 1 && file_select(fd_get(k, i), 1))) {
					ready(k);
					break;
				}
		}
	}
}

/* ---- switching ---- */

/*
 * The next ready process runs; the caller's IPL is 7.  The root stays
 * ready while others run, so the queue is empty only when the root
 * itself sleeps in a timeout function: then wait here for a waiter.
 */
static void
swtch_next(short sr)
{
	struct proc *p, *old = curproc;

	for (;;) {
		p = sysq[READY_Q].head;
		if (p)
			break;
		xa_splx(sr & ~0x700);
		poll_waiters();
		if (!sysq[READY_Q].head)
			xa_idle();
		xa_spl7();
	}
	rm_q(READY_Q, p);
	p->wait_q = CURPROC_Q;
	if (p == old)
		return;
	curproc = p;
	xa_swtch(&KP(old)->ksp, KP(p)->ksp);
}

/* the root: timeouts, waiters, then the others; stop when none is ready */
void
idle_loop(void)
{
	long lastdump = 0;

	for (;;) {
		run_timeouts();
		poll_waiters();
		if (xa_tracing && now_ms() - lastdump > 10000) {
			lastdump = now_ms();
			trace_procs();
		}
		if (sysq[READY_Q].head)
			sleep(READY_Q, 0);
		else
			xa_idle();
	}
}

/* the kernel's sleep: on que until woken; READY_Q yields */
int _cdecl
sleep(int que, long cond)
{
	short s = xa_spl7();
	struct kp *k = KP(curproc);

	if (que == READY_Q && !sysq[READY_Q].head) {
		xa_splx(s);
		return 0;
	}
	curproc->wait_cond = cond;
	add_q(que, curproc);
	swtch_next(s);
	xa_splx(s);
	if (k->kthread && (curproc->sigpending & ~curproc->p_sigmask)) {
		handle_sigs();
		return 1;
	}
	return 0;
}

void _cdecl
wake(int que, long cond)
{
	struct proc *p, *n;
	short s;

	if (que == READY_Q)
		return;
	s = xa_spl7();
	for (p = sysq[que].head; p; p = n) {
		n = p->q_next;
		if (p->wait_cond == cond) {
			rm_q(que, p);
			add_q(READY_Q, p);
			xa_resched = 1;
		}
	}
	xa_splx(s);
}

void _cdecl
iwake(int que, long cond, short pid)
{
	struct proc *p, *n;
	short s = xa_spl7();

	for (p = sysq[que].head; p; p = n) {
		n = p->q_next;
		if (p->wait_cond == cond && (pid < 0 || p->pid == pid)) {
			rm_q(que, p);
			add_q(READY_Q, p);
		}
	}
	xa_splx(s);
}

void _cdecl
wakeselect(struct proc *p)
{
	short s = xa_spl7();

	if (p->wait_q == SELECT_Q || p->wait_q == IO_Q) {
		rm_q(p->wait_q, p);
		add_q(READY_Q, p);
	}
	xa_splx(s);
}

void _cdecl
nap(unsigned n)
{
	KP(curproc)->wake_at = now_ms() + (n ? n : 1);
	sleep(WAIT_Q, (long)&KP(curproc)->wake_at);
	KP(curproc)->wake_at = 0;
}

long
xa_yield(void)
{
	sleep(READY_Q, 0);
	return 0;
}

/* Fselect's wait: 0 on timeout, 1 when woken */
int
sel_wait(long ms)
{
	struct kp *k = KP(curproc);

	k->wake_at = ms ? now_ms() + ms : 0;
	sleep(SELECT_Q, (long)k);
	ms = k->wake_at && now_ms() >= k->wake_at;
	k->wake_at = 0;
	return !ms;
}

/* the tick: ask for a switch when something is due */
void
xa_tick_c(void)
{
	if ((tlist && tlist->when <= now_ms()) || nsel || sysq[WAIT_Q].head)
		xa_resched = 1;
}

/* a new process's first run */
void
xa_kstart_c(void)
{
	struct kp *k = KP(curproc);
	CONTEXT *c = &curproc->ctxt[CURRENT];

	xa_splx(0x2000);
	k->started = 1;
	if (k->kthread) {
		(*k->kfunc)(k->karg);
		kthread_exit(0);
	}
	ks_pc = c->pc;
	ks_usp = c->usp;
	ks_sr = c->sr & 0xff1f;
	memcpy(ks_regs, c->regs, sizeof ks_regs);
}

/* ---- signals ---- */

static void sig_default(struct kp *k, int sig);

/* what a user handler interrupted, on the user's stack */
struct sigsave {
	long		regs[15];
	long		usp, pc;
	short		sr;
	ulong		mask;
	struct sigsave	*prev;
};

/*
 * Into a user handler on the way back to user mode: the context goes
 * on the user stack, under the handler's return to the trampoline,
 * the signal number, a zero vector offset and the context's address,
 * as MiNT lays them out.
 */
static void
sendsig(struct kp *k, int sig, short *frame)
{
	struct sigaction *sa = &k->p.p_sigacts->sigact[sig];
	long *regs = (long *)frame - 15, *sp;
	struct sigsave *ss;

	sp = (long *)((xa_usp() - sizeof *ss) & ~3L);
	ss = (struct sigsave *)sp;
	memcpy(ss->regs, regs, sizeof ss->regs);
	ss->usp = xa_usp();
	ss->pc = *(long *)(frame + 1);
	ss->sr = frame[0];
	ss->mask = k->p.p_sigmask;
	ss->prev = k->sigsave;
	k->sigsave = ss;
	*--sp = (long)ss;
	*--sp = 0;
	*--sp = sig;
	*--sp = (long)xa_sigtramp;
	xa_setusp((long)sp);
	*(long *)(frame + 1) = sa->sa_handler;
	k->p.p_sigmask |= sa->sa_mask;
	if (!(sa->sa_flags & SA_NODEFER))
		k->p.p_sigmask |= 1UL << sig;
	if (sa->sa_flags & SA_RESETHAND)
		sa->sa_handler = SIG_DFL;
}

/*
 * Psigreturn.  From the trampoline (the context's address is the
 * third argument): the handler's interrupted registers, PC and USP
 * come back.  Called by the program before a longjmp out of a
 * handler: only the mask does.
 */
long
xa_sigreturn(long *regs)
{
	struct kp *k = KP(curproc);
	struct sigsave *ss = k->sigsave;
	short *frame = (short *)(regs + 14);
	long *args = (long *)(xa_usp() + 2);

	if (!ss)
		return 0;
	k->sigsave = ss->prev;
	k->p.p_sigmask = ss->mask;
	if (args[2] != (long)ss)
		return 0;
	memcpy(regs, ss->regs + 1, 14 * sizeof(long));
	*(long *)(frame + 1) = ss->pc;
	frame[0] = (frame[0] & 0xff00) | (ss->sr & 0xff);
	xa_setusp(ss->usp);
	return ss->regs[0];
}

/* pending signals of the current process; frame: its way to user mode */
static void
handle_sigs_at(short *frame)
{
	struct kp *k = KP(curproc);
	ulong m;
	int sig;

	while ((m = curproc->sigpending & ~curproc->p_sigmask) != 0) {
		for (sig = 1; sig < NSIG && !(m & (1UL << sig)); sig++)
			;
		curproc->sigpending &= ~(1UL << sig);
		if (sig == SIGKILL || sig == SIGSTOP) {
			sig_default(k, sig);
			continue;
		}
		switch (curproc->p_sigacts->sigact[sig].sa_handler) {
		case SIG_IGN:
			break;
		case SIG_DFL:
			sig_default(k, sig);
			break;
		default:
			if (k->kthread)
				((void (*)(int))curproc->p_sigacts->sigact[sig].sa_handler)(sig);
			else if (frame) {
				sendsig(k, sig, frame);
				return;
			} else {
				curproc->sigpending |= 1UL << sig;
				return;
			}
		}
	}
}

static void
handle_sigs(void)
{
	handle_sigs_at(0);
}

static void
sig_default(struct kp *k, int sig)
{
	switch (sig) {
	case SIGCHLD: case SIGWINCH: case SIGCONT: case SIGURG: case SIGFPE:
	case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
		return;
	}
	xlog("pid %d (%s) ends by signal %d", k->p.pid, k->p.name, sig);
	proc_exit(k, sig << 8);
}

/* the trap return: due work, a switch if asked for, then signals */
void
xa_trapret(short *frame)
{
	if (xa_resched) {
		xa_resched = 0;
		run_timeouts();
		poll_waiters();
		xa_yield();
	}
	if (curproc->sigpending & ~curproc->p_sigmask)
		handle_sigs_at(frame);
}

long
post_sig(struct kp *k, int sig)
{
	if (sig < 0 || sig >= NSIG)
		return EBADARG;
	if (k->zombie || sig == 0)
		return 0;
	k->p.sigpending |= 1UL << sig;
	if (sig == SIGKILL || !(k->p.p_sigmask & (1UL << sig))) {
		if (k->p.wait_q != STOP_Q || k->started)
			ready(k);
	}
	return 0;
}

long _cdecl
ikill(int pid, unsigned short sig)
{
	struct proc *p = pid2proc(pid);

	if (!p)
		return ESRCH;
	return post_sig(KP(p), sig);
}

long _cdecl
killgroup(int pgrp, unsigned short sig, int priv)
{
	struct kp *k;
	long r = ESRCH;

	(void)priv;
	for (k = allprocs; k; k = k->all)
		if (k->p.pgrp == pgrp && !k->zombie && k->p.pid) {
			post_sig(k, sig);
			r = 0;
		}
	return r;
}

void _cdecl
raise(unsigned short sig)
{
	post_sig(KP(curproc), sig);
	handle_sigs();
}

/* ---- extensions ---- */

void *_cdecl
proc_lookup_extension(struct proc *p, long ident)
{
	struct p_ext *e;
	int i;

	if (!p)
		p = curproc;
	e = p->p_ext;
	for (i = 0; i < e->used; i++)
		if (e->ext[i]->ident == ident)
			return e->ext[i]->data;
	return 0;
}

void *_cdecl
proc_attach_extension(struct proc *p, long ident, unsigned long flags, unsigned long size,
	struct module_callback *cb)
{
	struct p_ext *e;
	struct proc_ext *x;

	if (!p)
		p = curproc;
	e = p->p_ext;
	if (e->used >= 10)
		return 0;
	x = kmalloc_o(sizeof *x, 0);
	if (!x)
		return 0;
	bzero(x, sizeof *x);
	x->data = kmalloc_o(size, 0);
	if (!x->data) {
		kfree_any(x);
		return 0;
	}
	bzero(x->data, size);
	x->ident = ident;
	x->flags = flags;
	x->size = size;
	x->links = 1;
	x->cb_vector = cb;
	e->ext[e->used++] = x;
	return x->data;
}

static void
ext_free(struct p_ext *e, int i)
{
	struct proc_ext *x = e->ext[i];

	if (x->cb_vector && x->cb_vector->release)
		(*x->cb_vector->release)(x->data);
	kfree_any(x->data);
	kfree_any(x);
	e->used--;
	for (; i < e->used; i++)
		e->ext[i] = e->ext[i + 1];
}

void _cdecl
proc_detach_extension(struct proc *p, long ident)
{
	struct p_ext *e;
	int i;

	if (!p)
		p = curproc;
	e = p->p_ext;
	for (i = 0; i < e->used; i++)
		if (e->ext[i]->ident == ident || ident == 0) {
			ext_free(e, i);
			i--;
		}
}

long _cdecl
proc_setuid(struct proc *p, unsigned short uid)
{
	p->p_cred->ucr->euid = p->p_cred->ruid = uid;
	return 0;
}

long _cdecl
proc_setgid(struct proc *p, unsigned short gid)
{
	p->p_cred->ucr->egid = p->p_cred->rgid = gid;
	return 0;
}

/* ---- threads and exit ---- */

long _cdecl
kthread_create(struct proc *p, void _cdecl (*func)(void *), void *arg, struct proc **np,
	const char *fmt, ...)
{
	struct kp *k;
	char name[PNAMSIZ + 1];
	va_list a;

	va_start(a, fmt);
	kvsprintf(name, sizeof name, fmt, a);
	va_end(a);
	k = proc_new(p ? KP(p) : KP(rootproc), name, 1, p != 0);
	if (!k)
		return ENOMEM;
	k->p.domain = DOM_MINT;
	k->kfunc = (void (*)(void *))func;
	k->karg = arg;
	if (np)
		*np = &k->p;
	xlog("kthread %d '%s'", k->p.pid, name);
	ready(k);
	return 0;
}

void _cdecl
kthread_exit(short code)
{
	proc_exit(KP(curproc), code);
}

/* the current process ends: callbacks, files, memory; its parent wakes */
void
proc_exit(struct kp *k, int code)
{
	struct p_ext *e = k->p.p_ext;
	struct kp *c;
	struct proc *pp;
	int i;
	short s;

	if (&k->p != curproc) {
		post_sig(k, SIGKILL);
		return;
	}
	xlog("exit pid %d (%s) code %d", k->p.pid, k->p.name, code);
	for (i = 0; i < e->used; i++)
		if (e->ext[i]->cb_vector && e->ext[i]->cb_vector->on_exit)
			(*e->ext[i]->cb_vector->on_exit)(e->ext[i]->data, &k->p, code);
	cancelproc(&k->p);
	fd_closeall(k);
	if (k->p.p_mem == &k->mem)
		mem_freeproc(k);
	else
		k->p.p_mem->links--;
	if (k->p.p_cwd != &k->cwd)
		k->p.p_cwd->links--;
	if (k->p.p_sigacts != &k->sa)
		k->p.p_sigacts->links--;
	for (c = allprocs; c; c = c->all)
		if (c->p.ppid == k->p.pid)
			c->p.ppid = 0;
	k->exitcode = code;
	k->zombie = 1;
	pp = pid2proc(k->p.ppid);
	if (pp) {
		wake(WAIT_Q, (long)pp);
		post_sig(KP(pp), SIGCHLD);
	}
	s = xa_spl7();
	add_q(ZOMBIE_Q, &k->p);
	swtch_next(s);
	for (;;)
		;
}

/* a zombie's structure goes */
void
proc_reap(struct kp *k)
{
	struct kp **kk;
	struct p_ext *e = k->p.p_ext;
	short s;

	while (e->used)
		ext_free(e, e->used - 1);
	s = xa_spl7();
	rm_q(ZOMBIE_Q, &k->p);
	for (kk = &allprocs; *kk; kk = &(*kk)->all)
		if (*kk == k) {
			*kk = k->all;
			break;
		}
	xa_splx(s);
	for (s = 0; s < 26; s++)
		if (k->dirs[s])
			kfree_any(k->dirs[s]);
	kfree_any(k);
}

/* zombies nobody waits for, orphans and threads, once no thread uses their parts */
void
reap_orphans(void)
{
	struct kp *k;

again:
	for (k = allprocs; k; k = k->all)
		if (k->zombie && &k->p != curproc && (k->p.ppid == 0 || k->kthread) &&
		    k->fd.links <= 1 && k->cwd.links <= 1 && k->mem.links <= 1 && k->sa.links <= 1) {
			proc_reap(k);
			goto again;
		}
}

unsigned long _cdecl
remaining_proc_time(void)
{
	return 20;
}
