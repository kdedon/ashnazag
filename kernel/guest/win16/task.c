/*
 * task.c -- several programs at once, as Windows 3.1 runs them: one
 * session in this process, the tasks taking turns.  A task gives the
 * CPU up only in GetMessage, PeekMessage, Yield and WinExec (when it has
 * nothing to do, or to let a new one start) and when it ends.
 *
 * Each task runs on its own host stack (an SVR4 ucontext): its own x86
 * registers and x87, its callbacks into x86 code nested on that stack,
 * the dispatcher's and USER's per-call state.  The scheduler is the
 * main context: a task yields to it, it picks the next.  The first task
 * is the shell: when it ends, the session does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ucontext.h>
#include "w16.h"
#include "win.h"

#define	STACK	(256 * 1024)
#define	NTASK	32

struct tctx {
	ucontext_t uc;
	char	*stack;
	jmp_buf	exitjb;
	struct x86 regs;	/* the CPU while another runs */
	char	*xc, *th;	/* the core's and the dispatcher's state */
	u32	us[2];		/* USER's scratch */
};

struct task *curtask, *tasks[NTASK];
int ntasks;
static struct tctx mainx;	/* the scheduler's own state, while a task runs */
int task_endsession;		/* ExitWindows: every task ends with the session */
static ucontext_t mainuc;
static int rr;			/* where the round goes on */

extern int x86_ctxsize(), thunk_ctxsize();
extern void x86_ctxsave(), x86_ctxload(), thunk_ctxnew(), thunk_ctxsave(), thunk_ctxload(), thunk_ctxfree();
extern void user_ctxsave(), user_ctxload(), user_taskended(), ws_taskended();

#define	TC(t)	((struct tctx *)(t)->t_ctx)

static void
save(t)
	struct task *t;
{
	struct tctx *x = TC(t);

	x->regs = *cpu;
	x86_ctxsave(x->xc);
	thunk_ctxsave(x->th);
	user_ctxsave(x->us);
}

static void
load(t)
	struct task *t;
{
	struct tctx *x = TC(t);

	*cpu = x->regs;
	x86_ctxload(x->xc);
	thunk_ctxload(x->th);
	user_ctxload(x->us);
	curtask = t;
}

/* a task's life: its program from the entry point to the end */
static void
entry()
{
	struct task *t = curtask;

	if (setjmp(TC(t)->exitjb) == 0) {
		x86_run(cpu);
		w16_log("startwin: %s stopped at %04x:%04x\n", t->t_mod->m_name, cpu->s[S_CS].sel,
		    cpu->eip & 0xffff);
		t->t_exit = 255;
	}
	t->t_done = 1;
	setcontext(&mainuc);
}

/*
 * A new task for program m, registers as Windows starts a program; it
 * runs when the scheduler comes to it.
 */
struct task *
task_new(m, cmdline, show)
	struct module *m;
	char *cmdline;
	int show;
{
	extern u16 psp_make();
	struct task *t;
	struct tctx *x;
	struct x86 keep;
	u32 sp, dg;
	int i;

	for (i = 0; i < NTASK && tasks[i]; i++)
		;
	if (i == NTASK || !m->m_cs || m->m_cs > m->m_nseg || !m->m_ss || m->m_ss > m->m_nseg)
		return 0;
	t = (struct task *)calloc(1, sizeof *t);
	x = (struct tctx *)calloc(1, sizeof *x);
	t->t_ctx = (void *)x;
	t->t_mod = m;
	t->t_hmod = m->m_hmod;
	t->t_hinst = m->m_hinst;
	t->t_show = show;
	t->t_events = 1;		/* as Windows starts a task: its startup's WaitEvent(0) returns */
	strncpy(t->t_cmdline, cmdline, sizeof t->t_cmdline - 1);
	t->t_psp = psp_make(cmdline, m->m_hinst);
	t->t_dta = FP(t->t_psp, 0x80);
	t->t_htask = g_alloc(GMEM_ZEROINIT, 0x100, m->m_hinst);
	PB(sel_base(t->t_htask) + 0xfa, 'T');
	PB(sel_base(t->t_htask) + 0xfb, 'D');
	PW(sel_base(t->t_htask) + 0x1c, m->m_hinst);
	PW(sel_base(t->t_htask) + 0x1e, m->m_hmod);
	/* the registers, made on the CPU and kept */
	keep = *cpu;
	dg = m->m_dgroup ? m->m_seg[m->m_dgroup].ns_sel : 0;
	x86_loadseg(cpu, S_SS, m->m_seg[m->m_ss].ns_sel);
	sp = m->m_sp;
	if (sp == 0) {
		sp = (m->m_ss == m->m_dgroup ? m->m_seg[m->m_ss].ns_alloc - m->m_heap :
		    m->m_seg[m->m_ss].ns_alloc) & ~1;
		if (sp > 0xfffe)
			sp = 0xfffe;
	}
	cpu->r[R_SP] = sp;
	cpu->r[R_AX] = 0;
	cpu->r[R_BX] = m->m_stack;
	cpu->r[R_CX] = m->m_heap;
	cpu->r[R_DX] = 0;
	cpu->r[R_SI] = 0;
	cpu->r[R_DI] = m->m_hinst;
	cpu->r[R_BP] = 0;
	cpu->depth = 0;
	cpu->stop = 0;
	x86_loadseg(cpu, S_DS, dg);
	x86_loadseg(cpu, S_ES, t->t_psp);
	x86_loadseg(cpu, S_FS, 0);
	x86_loadseg(cpu, S_GS, 0);
	x86_loadseg(cpu, S_CS, m->m_seg[m->m_cs].ns_sel);
	cpu->eip = m->m_ip;
	cpu->x87 = 0;
	x87_init(cpu);			/* its own coprocessor */
	x->regs = *cpu;
	*cpu = keep;
	x->xc = (char *)calloc(1, x86_ctxsize());
	x->th = (char *)calloc(1, thunk_ctxsize());
	thunk_ctxnew(x->th);
	/* its stack */
	x->stack = (char *)malloc(STACK);
	getcontext(&x->uc);
	x->uc.uc_stack.ss_sp = x->stack;
	x->uc.uc_stack.ss_size = STACK;
	x->uc.uc_stack.ss_flags = 0;
	x->uc.uc_link = &mainuc;
	makecontext(&x->uc, entry, 0);
	t->t_new = 1;
	tasks[i] = t;
	ntasks++;
	return t;
}

/* to the scheduler; back when this task's turn comes again */
void
task_yield()
{
	struct task *t = curtask;

	if (!t || ntasks < 2)
		return;
	swapcontext(&TC(t)->uc, &mainuc);
}

int
task_alive(t)
	struct task *t;
{
	int i;

	for (i = 0; i < NTASK; i++)
		if (tasks[i] == t)
			return !t->t_done;
	return 0;
}

/* another task with something to do (a new one, or one not waiting idle) */
int
task_othersready()
{
	int i;

	for (i = 0; i < NTASK; i++)
		if (tasks[i] && tasks[i] != curtask && !tasks[i]->t_done && (tasks[i]->t_new || !tasks[i]->t_idle))
			return 1;
	return 0;
}

/* the task ends now (ExitProgram, INT 21h 4Ch, the fault box) */
void
w16_exit(code)
	int code;
{
	struct task *t = curtask;

	if (!t)
		exit(code);
	t->t_exit = code;
	t->t_done = 1;
	longjmp(TC(t)->exitjb, 1);
}

static void
ended(t)
	struct task *t;
{
	struct tctx *x = TC(t);
	int i;

	user_taskended(t);
	ws_taskended(t);
	for (i = 0; i < NTASK; i++)
		if (tasks[i] == t)
			tasks[i] = 0;
	ntasks--;
	thunk_ctxfree(x->th);
	free(x->xc);
	free(x->th);
	free(x->stack);
	if (x->regs.x87)
		free(x->regs.x87);
	if (x->us[0])
		g_free(x->us[0]);
	free((char *)x);
	free((char *)t);
}

/* the session: the tasks in turn until the first (the shell) ends; its exit code */
int
task_run(first)
	struct task *first;
{
	struct task *t;
	int i, k, code = 0;

	for (;;) {
		t = 0;
		for (k = 0; k < NTASK && !t; k++) {
			i = (rr + k) % NTASK;
			if (tasks[i] && !tasks[i]->t_done)
				t = tasks[i];
		}
		if (!t)
			break;
		rr = (i + 1) % NTASK;
		/*
		 * The scheduler's CPU and dispatcher state kept aside and put
		 * back: a task that ends frees its callback stack, which the
		 * globals still name (DLL initialisation after it wrote there).
		 */
		if (!mainx.xc) {
			mainx.xc = (char *)calloc(1, x86_ctxsize());
			mainx.th = (char *)calloc(1, thunk_ctxsize());
			thunk_ctxnew(mainx.th);
		}
		mainx.regs = *cpu;
		x86_ctxsave(mainx.xc);
		thunk_ctxsave(mainx.th);
		user_ctxsave(mainx.us);
		load(t);
		t->t_new = 0;
		if (w16_debug > 2)
			w16_log("startwin: to %s\n", t->t_mod->m_name);
		swapcontext(&mainuc, &TC(t)->uc);
		save(t);
		*cpu = mainx.regs;
		x86_ctxload(mainx.xc);
		thunk_ctxload(mainx.th);
		user_ctxload(mainx.us);
		curtask = 0;
		if (t->t_done) {
			if (w16_debug)
				w16_log("startwin: %s ended (%d)\n", t->t_mod->m_name, t->t_exit);
			if (t == first || task_endsession) {
				code = t == first ? t->t_exit : 0;
				ended(t);
				break;
			}
			ended(t);
		}
	}
	return code;
}
