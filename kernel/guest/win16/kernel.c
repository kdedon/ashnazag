/*
 * kernel.c -- our KERNEL: tasks, memory, modules, resources, files,
 * profiles, atoms and strings for Win16 programs.
 *
 * One task runs at a time; WinExec of a second program is not yet
 * there (see docs/win16-design.md).  Handlers take their arguments
 * first argument first (thunk.c) and return DX:AX.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include "w16.h"
#include "apitab.h"

int w16_debug;
char windir[128] = "C:\\WINDOWS", sysdir[128] = "C:\\WINDOWS\\SYSTEM";

static u16 lowsel[2], dummysel;
static u16 envsel;
static struct timeval t0;
extern int api_jumped;

#define	STR(p)		(gptr(p) ? gptr(p) : "")
#define	ISINT(p)	(FPSEL(p) == 0)

/* ---- logging ---- */

void
w16_log(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void
w16_fatal(char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "startwin: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (cpu)
		fprintf(stderr, " (at %04x:%04x)", cpu->s[S_CS].sel, cpu->eip & 0xffff);
	fprintf(stderr, "\n");
	exit(3);
}

u32
w16_ticks()
{
	struct timeval tv;

	gettimeofday(&tv, (struct timezone *)0);
	return (tv.tv_sec - t0.tv_sec) * 1000 + (tv.tv_usec - t0.tv_usec) / 1000;
}

/* ---- the system's selectors ---- */

u32
kernel_lowsel(name)
	char *name;
{
	if (strcmp(name, "__0000H") == 0)
		return lowsel[0];
	if (strcmp(name, "__0040H") == 0)
		return lowsel[1];
	return dummysel;
}

u32
kernel_winflags()
{
	return 0x0015 | (cpu && cpu->fpu ? 0x0400 : 0);	/* protected, 386, standard mode */
}

void
kernel_init()
{
	char *e;
	u32 b;

	gettimeofday(&t0, (struct timezone *)0);
	lowsel[0] = sel_alloc(1);
	sel_set(lowsel[0], 0, 0xffff, D_P | D_S | D_W | D_A);
	lowsel[1] = sel_alloc(1);
	sel_set(lowsel[1], 0x400, 0xfbff, D_P | D_S | D_W | D_A);
	dummysel = g_alloc(GMEM_ZEROINIT, 0x10000, 0);
	/* the BIOS data area a program may look at: screen columns, rows */
	PW(0x44a, 80);
	PB(0x484, 24);
	/* the DOS environment */
	envsel = g_alloc(GMEM_ZEROINIT, 1024, 0);
	b = sel_base(envsel);
	e = (char *)M + b;
	sprintf(e, "PATH=%s;%s", windir, sysdir);
	e += strlen(e) + 1;
	sprintf(e, "COMSPEC=C:\\COMMAND.COM");
	e += strlen(e) + 1;
	sprintf(e, "TEMP=C:\\TEMP");
	e += strlen(e) + 1;
	sprintf(e, "windir=%s", windir);
	e += strlen(e) + 1;
	*e++ = 0;
	PW(e - (char *)M, 1);		/* then the program's name */
}

/* the BIOS tick count a program may read at 0040:006C */
void
kernel_tick()
{
	PL(0x46c, w16_ticks() * 182 / 10000);
}

/* ---- tasks (task.c runs them) ---- */

/* a raw command tail for the next PSP (LoadModule), -1 none */
static char rawtail[128];
static int rawlen = -1;

u16
psp_make(cmdline, hinst)
	char *cmdline;
	int hinst;
{
	u16 psp = g_alloc(GMEM_ZEROINIT, 256, hinst);
	u32 b = sel_base(psp);
	int n = strlen(cmdline);

	/* LoadModule's tail as given, bytes and all: MMTASK.TSK takes a binary one (mmTaskCreate's) */
	if (rawlen >= 0) {
		PB(b, 0xcd);
		PB(b + 1, 0x20);
		PW(b + 2, 0x9fff);
		PW(b + 0x2c, envsel);
		PB(b + 0x80, rawlen);
		memcpy(M + b + 0x81, rawtail, rawlen);
		PB(b + 0x81 + rawlen, 0x0d);
		rawlen = -1;
		return psp;
	}
	if (n > 126)
		n = 126;
	PB(b, 0xcd);
	PB(b + 1, 0x20);
	PW(b + 2, 0x9fff);
	PW(b + 0x2c, envsel);
	PB(b + 0x80, n);
	memcpy(M + b + 0x81, cmdline, n);
	/* DOS ends the tail with CR; WinMain gets it as a C string, as InitTask leaves it */
	PB(b + 0x81 + n, 0);
	return psp;
}

/* InitTask: register entry; the local heap, then what the startup code wants */
static u32
k_InitTask(a)
	u32 *a;
{
	struct task *t = curtask;
	struct module *m = t->t_mod;
	u32 ds = cpu->s[S_DS].sel, b = sel_base(ds), sp = cpu->r[R_SP] & 0xffff;
	u32 top, end;

	if (m->m_dgroup && m->m_heap) {
		top = m->m_seg[m->m_dgroup].ns_alloc - m->m_heap;
		end = m->m_seg[m->m_dgroup].ns_alloc - 1;
		l_init(ds, top, end);
	}
	/* the instance data: stack top, limit and bottom */
	PW(b + 0x0a, m->m_stack ? sp - m->m_stack + 0x10 : 0x10);
	PW(b + 0x0c, m->m_stack ? sp - m->m_stack + 0x10 : 0x10);
	PW(b + 0x0e, sp);
	cpu->r[R_AX] = 1;
	cpu->r[R_BX] = 0x81;
	cpu->r[R_CX] = GW(b + 0x0a);
	cpu->r[R_DX] = t->t_show;
	cpu->r[R_SI] = 0;
	cpu->r[R_DI] = t->t_hinst;
	x86_loadseg(cpu, S_ES, t->t_psp);
	return 1;
}

/*
 * WaitEvent: an event already posted is taken at once (FALSE); else the
 * task waits for one (others run meanwhile) and takes it (TRUE).
 * PostEvent: one for a task, which wakes if it waits.  MMSYSTEM's
 * mmTaskBlock and mmTaskSignal are these (MCIWAVE's playing task).
 */
static u32
k_WaitEvent(a)
	u32 *a;
{
	extern void user_idle();
	struct task *t = curtask;
	int i;

	for (i = 0; LO16(a[0]) && i < NTASK_MAX; i++)
		if (tasks[i] && tasks[i]->t_htask == LO16(a[0]))
			t = tasks[i];
	if (!t)
		return 0;
	if (t->t_events > 0) {
		t->t_events--;
		return 0;
	}
	while (t->t_events <= 0 && !t->t_done)
		user_idle();
	if (t->t_events > 0)
		t->t_events--;
	return 1;
}

static u32
k_PostEvent(a)
	u32 *a;
{
	int i;

	for (i = 0; i < NTASK_MAX; i++)
		if (tasks[i] && tasks[i]->t_htask == LO16(a[0])) {
			tasks[i]->t_events++;
			tasks[i]->t_idle = 0;
			return 0;
		}
	return 0;
}
static u32 k_Yield(a) u32 *a; { extern void user_yield(); user_yield(); return 0; }
static u32 k_GetVersion(a) u32 *a; { return 0x05000a03; }
static u32 k_GetWinFlags(a) u32 *a; { return kernel_winflags(); }
static u32 k_GetCurrentTask(a) u32 *a; { return curtask ? curtask->t_htask : 0; }
static u32 k_GetCurrentPDB(a) u32 *a; { return curtask ? curtask->t_psp : 0; }
static u32 k_GetNumTasks(a) u32 *a; { return ntasks; }

/* IsTaskLocked: no task holds the CPU to itself (LockCurrentTask) */
static u32 k_IsTaskLocked(a) u32 *a; { return 0; }

static u32
k_IsTask(a)
	u32 *a;
{
	int i;

	for (i = 0; i < 32; i++)
		if (tasks[i] && !tasks[i]->t_done && tasks[i]->t_htask == LO16(a[0]))
			return 1;
	return 0;
}
static u32 k_GetTaskQueue(a) u32 *a; { return curtask ? curtask->t_queue : 0; }

static u32
k_SetTaskQueue(a)
	u32 *a;
{
	u16 o = curtask ? curtask->t_queue : 0;

	if (curtask)
		curtask->t_queue = a[1];
	return o;
}

static u32
k_GetDOSEnvironment(a)
	u32 *a;
{
	return FP(envsel, 0);
}

static u32
k_GetExePtr(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);

	return m ? m->m_hmod : 0;
}

static u32
k_GetModuleHandle(a)
	u32 *a;
{
	struct module *m;
	char name[16], *s, *p;
	int i;

	if (ISINT(a[0])) {
		m = mod_byhandle(FPOFF(a[0]));
		return m ? m->m_hmod : 0;
	}
	s = STR(a[0]);
	if ((p = strrchr(s, '\\')) != 0)
		s = p + 1;
	for (i = 0; s[i] && s[i] != '.' && i < 15; i++)
		name[i] = s[i];
	name[i] = 0;
	m = mod_find(name);
	return m ? m->m_hmod : 0;
}

static u32
k_GetModuleUsage(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);

	return m ? m->m_ref : 0;
}

static u32
k_GetModuleFileName(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	char *d = gptr(a[1]);
	int n = (short)a[2];

	if (!m || !d || n <= 0)
		return 0;
	strncpy(d, m->m_path, n - 1);
	d[n - 1] = 0;
	return strlen(d);
}

static u32
k_GetModuleName(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	char *d = gptr(a[1]);
	int n = a[2];

	if (!m || !d || n <= 0)
		return 0;
	strncpy(d, m->m_name, n - 1);
	d[n - 1] = 0;
	return 1;
}

static u32
k_GetProcAddress(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);

	if (!m)
		return 0;
	if (w16_debug > 1)
		w16_log("startwin: GetProcAddress %s %s%s\n", m->m_name, ISINT(a[1]) ? "#" : "",
		    ISINT(a[1]) ? "" : STR(a[1]));
	if (ISINT(a[1]))
		return drv_proc(m, mod_proc(m, FPOFF(a[1]), (char *)0));
	return drv_proc(m, mod_proc(m, 0, STR(a[1])));
}

/* run a DLL's entry point (LibMain): CX heap, DI hInstance, DS DGROUP, ES:SI command line */
static int
libinit(m)
	struct module *m;
{
	u32 r;

	if (!m->m_dll || m->m_native || !m->m_cs)
		return 1;
	if (m->m_dgroup && m->m_heap)
		l_init(m->m_hinst, m->m_seg[m->m_dgroup].ns_alloc - m->m_heap,
		    m->m_seg[m->m_dgroup].ns_alloc - 1);
	cb_begin();
	cpu->r[R_CX] = m->m_heap;
	cpu->r[R_DI] = m->m_hinst;
	cpu->r[R_SI] = 0;
	x86_loadseg(cpu, S_ES, 0);
	r = cb_call(FP(m->m_seg[m->m_cs].ns_sel, m->m_ip), m->m_dgroup ? m->m_hinst : 0);
	return (r & 0xffff) != 0;
}

/* the DLLs a module needs, from the bottom, then itself */
void
initdeps(m, depth)
	struct module *m;
	int depth;
{
	int i;

	/* once a module: a DLL two others import is initialised all the same (SHELL) */
	if (depth > 16 || m->m_native || m->m_init)
		return;
	m->m_init = 1;
	for (i = 0; i < m->m_nimp; i++)
		if (m->m_imp[i] && !m->m_imp[i]->m_native)
			initdeps(m->m_imp[i], depth + 1);
	if (m->m_dll && !(m->m_flags & 0x4000))
		libinit(m);
}

static u32
k_LoadLibrary(a)
	u32 *a;
{
	struct module *m;
	int err;

	m = mod_load(STR(a[0]), &err);
	if (!m)
		return err ? err : 2;
	initdeps(m, 0);
	return m->m_hinst;
}

static u32
k_FreeLibrary(a)
	u32 *a;
{
	mod_free(mod_byhandle(a[0]));
	return 0;
}

static u32
k_FreeModule(a)
	u32 *a;
{
	mod_free(mod_byhandle(a[0]));
	return 1;
}

static u32
k_MakeProcInstance(a)
	u32 *a;
{
	return thunk_procinst(a[0], LO16(a[1]) ? mod_byhandle(a[1]) ? mod_byhandle(a[1])->m_hinst : a[1] : 0);
}

static u32
k_FreeProcInstance(a)
	u32 *a;
{
	extern void thunk_freeinst();

	thunk_freeinst(a[0]);
	return 0;
}

static u32
k_GetInstanceData(a)
	u32 *a;
{
	u32 s = lin(a[0], 0), d = lin(api_callerds, 0);

	if (!s || !d)
		return 0;
	memmove(M + d + LO16(a[1]), M + s + LO16(a[1]), LO16(a[2]));
	return a[2];
}

static u32
k_GetExpWinVer(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);

	if (!m || m->m_native)
		return 0x030a;
	return GW(sel_base(m->m_hmod) + 0x3e) ? GW(sel_base(m->m_hmod) + 0x3e) : 0x0300;
}

/* ---- global memory ---- */

static u32
k_GlobalAlloc(a)
	u32 *a;
{
	return g_alloc(a[0], a[1], curtask ? curtask->t_hinst : 0);
}

static u32
k_GlobalReAlloc(a)
	u32 *a;
{
	return g_realloc(a[0], a[1], a[2]);
}

static u32
k_GlobalFree(a)
	u32 *a;
{
	return g_free(a[0]);
}

static u32
k_GlobalLock(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);

	if (!b || b->gb_discarded)
		return 0;
	b->gb_lock++;
	return FP(SEL(b - gblk), 0);
}

static u32
k_GlobalUnlock(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);

	if (!b)
		return 0;
	if (b->gb_lock)
		b->gb_lock--;
	return b->gb_lock != 0;
}

static u32 k_GlobalSize(a) u32 *a; { return g_size(a[0]); }

static u32
k_GlobalHandle(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);

	return b ? FP(SEL(b - gblk), SEL(b - gblk)) : 0;
}

static u32
k_GlobalFlags(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);

	if (!b)
		return 0;
	return (b->gb_lock & 0xff) | (b->gb_flags & GMEM_DISCARDABLE) | (b->gb_discarded ? GMEM_DISCARDED : 0);
}

static u32 k_GlobalCompact(a) u32 *a; { return g_free_bytes(); }
static u32 k_GetFreeSpace(a) u32 *a; { return g_free_bytes(); }
static u32 k_GlobalFix(a) u32 *a; { return a[0]; }
static u32 k_LockSegment(a) u32 *a; { return LO16(a[0]) == 0xffff ? api_callerds : a[0]; }
static u32 k_UnlockSegment(a) u32 *a; { return 0; }
static u32 k_GlobalWire(a) u32 *a; { return FP(a[0], 0); }
static u32 k_GlobalUnWire(a) u32 *a; { return 1; }
static u32 k_GlobalPageLock(a) u32 *a; { return 1; }
static u32 k_GlobalLRU(a) u32 *a; { return a[0]; }

static u32
k_GlobalDOSAlloc(a)
	u32 *a;
{
	u16 s = g_alloc(GMEM_ZEROINIT, a[0], curtask ? curtask->t_hinst : 0);

	return s ? FP(sel_base(s) >> 4, s) : 0;
}

static u32
k_AllocSelector(a)
	u32 *a;
{
	u16 s = sel_alloc(1);

	if (s && LO16(a[0]))
		LDT[SELIX(s)] = LDT[SELIX(a[0])];
	else if (s)
		sel_set(s, 0, 0, D_P | D_S | D_W);
	return s;
}

static u32
k_AllocSelectorArray(a)
	u32 *a;
{
	u16 s = sel_alloc(a[0]);
	int i;

	for (i = 0; s && i < (int)a[0]; i++)
		sel_set(s + 8 * i, 0, 0, D_P | D_S | D_W);
	return s;
}

static u32
k_FreeSelector(a)
	u32 *a;
{
	if (g_block(a[0]))
		return a[0];
	sel_free(a[0], 1);
	return 0;
}

static u32 k_AllocCStoDSAlias(a) u32 *a; { return sel_alias(a[0], 0); }
static u32 k_AllocDStoCSAlias(a) u32 *a; { return sel_alias(a[0], 1); }

static u32
k_PrestoChangoSelector(a)
	u32 *a;
{
	struct desc *s = &LDT[SELIX(a[0])], *d = &LDT[SELIX(a[1])];

	d->d_base = s->d_base;
	d->d_limit = s->d_limit;
	d->d_acc = (s->d_acc & D_CODE) ? (s->d_acc & ~(D_CODE | D_R)) | D_W : (s->d_acc & ~D_W) | D_CODE | D_R;
	return a[1];
}

static u32 k_GetSelectorBase(a) u32 *a; { return sel_base(a[0]); }
static u32 k_GetSelectorLimit(a) u32 *a; { return LDT[SELIX(a[0])].d_limit; }

static void
reloadsegs(sel)
	int sel;
{
	int i;

	for (i = 0; i < 6; i++)
		if (cpu->s[i].sel == (sel | 7) || cpu->s[i].sel == sel)
			x86_loadseg(cpu, i, cpu->s[i].sel);
}

static u32
k_SetSelectorBase(a)
	u32 *a;
{
	LDT[SELIX(a[0])].d_base = a[1];
	reloadsegs(a[0]);
	return a[0];
}

static u32
k_SetSelectorLimit(a)
	u32 *a;
{
	LDT[SELIX(a[0])].d_limit = a[1];
	reloadsegs(a[0]);
	return 0;
}

static u32
k_SelectorAccessRights(a)
	u32 *a;
{
	struct desc *d = &LDT[SELIX(a[0])];

	if (a[1])
		d->d_acc = (d->d_acc & ~0xff) | (a[2] & 0xff & ~D_DPL);
	return d->d_acc | 0x60;
}

static u32
k_LongPtrAdd(a)
	u32 *a;
{
	struct desc *d = &LDT[SELIX(FPSEL(a[0]))];

	d->d_base += a[1];
	reloadsegs(FPSEL(a[0]));
	return a[0];
}

static u32
k_hmemcpy(a)
	u32 *a;
{
	u32 d = lin(FPSEL(a[0]), FPOFF(a[0])), s = lin(FPSEL(a[1]), FPOFF(a[1]));

	/* huge pointers: the selectors go on 64K at a time, so linear is contiguous */
	if (d && s && d + a[2] <= MSIZE && s + a[2] <= MSIZE)
		memmove(M + d, M + s, a[2]);
	return 0;
}

static u32
k_IsBadPtr(a)
	u32 *a;
{
	u32 sel = FPSEL(a[0]), off = FPOFF(a[0]), n = a[1] & 0xffff;
	struct desc *d;

	if (!(sel & 4) || !SELIX(sel) || SELIX(sel) >= LDTSIZE)
		return 1;
	d = &LDT[SELIX(sel)];
	if (!(d->d_acc & D_P))
		return 1;
	return n && off + n - 1 > d->d_limit;
}

static u32
k_IsBadStringPtr(a)
	u32 *a;
{
	u32 sel = FPSEL(a[0]), off = FPOFF(a[0]);
	struct desc *d;

	if (!(sel & 4) || !SELIX(sel) || SELIX(sel) >= LDTSIZE)
		return 1;
	d = &LDT[SELIX(sel)];
	return !(d->d_acc & D_P) || off > d->d_limit;
}

/* ---- local memory: in the caller's DS ---- */

static u32 k_LocalInit(a) u32 *a; { return l_init(LO16(a[0]) ? a[0] : api_callerds, a[1], a[2]); }
static u32 k_LocalAlloc(a) u32 *a; { return l_alloc(api_callerds, a[0], a[1]); }
static u32 k_LocalReAlloc(a) u32 *a; { return l_realloc(api_callerds, a[0], a[1], a[2]); }
static u32 k_LocalFree(a) u32 *a; { return l_free(api_callerds, a[0]); }
static u32
k_LocalLock(a)
	u32 *a;
{
	u32 r = l_lock(api_callerds, a[0]);

	if (!r && w16_debug > 1)
		w16_log("startwin: LocalLock(%lx) in DS %x: no such block\n", (long)a[0], api_callerds);
	return r;
}
static u32 k_LocalUnlock(a) u32 *a; { return l_unlock(api_callerds, a[0]); }
static u32 k_LocalSize(a) u32 *a; { return l_size(api_callerds, a[0]); }
static u32 k_LocalHandle(a) u32 *a; { return l_handle(api_callerds, a[0]); }
static u32 k_LocalFlags(a) u32 *a; { return l_flags(api_callerds, a[0]); }
static u32 k_LocalCompact(a) u32 *a; { return l_compact(api_callerds, a[0]); }
static u32 k_LocalShrink(a) u32 *a; { return 0x1000; }

/* ---- resources ---- */

static u32
k_FindResource(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0] ? a[0] : curtask ? curtask->t_hinst : 0);
	u32 h = res_find(m, a[2], a[1]);

	return h ? h : 0;
}

/* a loaded resource's global handle, by module and entry, so FreeResource can find it */
#define	NLOADED	512
static struct {
	struct module *m;
	u16 hres, g;
	u16 refs;
} loaded[NLOADED];

static u32
k_LoadResource(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0] ? a[0] : curtask ? curtask->t_hinst : 0);
	int i, f = -1;
	u16 g;

	if (!m || !a[1])
		return 0;
	for (i = 0; i < NLOADED; i++) {
		if (loaded[i].m == m && loaded[i].hres == a[1] && g_block(loaded[i].g)) {
			loaded[i].refs++;
			return loaded[i].g;
		}
		if (!loaded[i].m && f < 0)
			f = i;
	}
	g = res_load(m, a[1]);
	if (g && f >= 0) {
		loaded[f].m = m;
		loaded[f].hres = a[1];
		loaded[f].g = g;
		loaded[f].refs = 1;
	}
	return g;
}

static u32
k_LockResource(a)
	u32 *a;
{
	struct gblock *b = g_block(a[0]);

	return b && !b->gb_discarded ? FP(SEL(b - gblk), 0) : 0;
}

static u32
k_FreeResource(a)
	u32 *a;
{
	int i;

	for (i = 0; i < NLOADED; i++)
		if (loaded[i].m && loaded[i].g == LO16(a[0])) {
			if (--loaded[i].refs == 0) {
				g_free(loaded[i].g);
				loaded[i].m = 0;
			}
			return 0;
		}
	return a[0];
}

static u32
k_SizeofResource(a)
	u32 *a;
{
	return res_size(mod_byhandle(a[0]), a[1]);
}

static u32
k_AccessResource(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);
	int h, shift;
	u32 off;

	if (!m || !a[1])
		return -1;
	if ((h = dos_open(m->m_path, 0)) < 0)
		return -1;
	shift = GW(sel_base(m->m_hmod) + GW(sel_base(m->m_hmod) + 0x24));
	off = (u32)GW(sel_base(m->m_hmod) + a[1]) << shift;
	dos_seek(h, off, 0);
	return h;
}

static u32
k_AllocResource(a)
	u32 *a;
{
	return g_alloc(GMEM_MOVEABLE, a[2] ? a[2] : res_size(mod_byhandle(a[0]), a[1]), a[0]);
}

/* DirectResAlloc(hInstance, flags, size): a resource's memory, the module's */
static u32
k_DirectResAlloc(a)
	u32 *a;
{
	struct module *m = mod_byhandle(a[0]);

	return g_alloc(GMEM_MOVEABLE, a[2], m ? m->m_hmod : a[0]);
}

static u32 k_SetResourceHandler(a) u32 *a; { return 0; }

/* ---- files ---- */

static u32
k_lopen(a)
	u32 *a;
{
	int h = dos_open(STR(a[0]), a[1]);

	return h < 0 ? 0xffff : h;
}

static u32
k_lcreat(a)
	u32 *a;
{
	int h = dos_creat(STR(a[0]), a[1]);

	return h < 0 ? 0xffff : h;
}

static u32 k_lclose(a) u32 *a; { return dos_close(a[0]) < 0 ? 0xffff : 0; }

static u32
k_lread(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	s32 r;

	if (!l && a[2])
		return 0xffff;
	r = dos_read(a[0], l, a[2] & 0xffff);
	return r < 0 ? 0xffff : r;
}

static u32
k_lwrite(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	s32 r;

	if (!l && a[2])
		return 0xffff;
	r = dos_write(a[0], l, a[2] & 0xffff);
	return r < 0 ? 0xffff : r;
}

static u32
k_hread(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	s32 r;

	if (!l && a[2])
		return 0xffffffff;
	r = dos_read(a[0], l, a[2]);
	return r < 0 ? 0xffffffff : r;
}

static u32
k_hwrite(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	s32 r;

	if (!l && a[2])
		return 0xffffffff;
	r = dos_write(a[0], l, a[2]);
	return r < 0 ? 0xffffffff : r;
}

static u32
k_llseek(a)
	u32 *a;
{
	s32 r = dos_seek(a[0], (s32)a[1], a[2]);

	return r < 0 ? 0xffffffff : r;
}

/* OpenFile: OFSTRUCT is cBytes, fFixedDisk, nErrCode, reserved[4], szPathName[128] */
#define	OF_READ		0x0000
#define	OF_PARSE	0x0100
#define	OF_DELETE	0x0200
#define	OF_VERIFY	0x0400
#define	OF_SEARCH	0x0400
#define	OF_CANCEL	0x0800
#define	OF_CREATE	0x1000
#define	OF_PROMPT	0x2000
#define	OF_EXIST	0x4000
#define	OF_REOPEN	0x8000

static u32
k_OpenFile(a)
	u32 *a;
{
	char *name = STR(a[0]), *of = gptr(a[1]), full[300], host[1024], try[300];
	int mode = a[2], h = -2, i;
	char *dirs[3];

	if (!of)
		return 0xffff;
	if (mode & OF_REOPEN)
		name = of + 8;
	memset(of, 0, 8);
	of[0] = 136;
	of[1] = 1;
	if (dos_fullpath(name, full) != 0) {
		PW(of - (char *)M + 2, 3);
		return 0xffff;
	}
	/* a plain name not here: the Windows and system directories */
	if (!strchr(name, '\\') && !strchr(name, ':') && !(mode & OF_CREATE) &&
	    dos_hostpath(full, host, sizeof host, 0) != 0) {
		dirs[0] = windir;
		dirs[1] = sysdir;
		dirs[2] = 0;
		for (i = 0; dirs[i]; i++) {
			sprintf(try, "%s\\%s", dirs[i], name);
			if (dos_fullpath(try, try) == 0 && dos_hostpath(try, host, sizeof host, 0) == 0) {
				strcpy(full, try);
				break;
			}
		}
	}
	strcpy(of + 8, full);
	if (mode & OF_PARSE)
		return 0;
	if (mode & OF_DELETE) {
		if ((i = dos_delete(full)) != 0) {
			PW(of - (char *)M + 2, i);
			return 0xffff;
		}
		return 1;
	}
	if (mode & OF_CREATE)
		h = dos_creat(full, 0);
	else
		h = dos_open(full, mode & 3);
	if (h < 0) {
		PW(of - (char *)M + 2, -h);
		return 0xffff;
	}
	if (mode & OF_EXIST) {
		dos_close(h);
		return 1;
	}
	return h;
}

u32
kernel_openfile(name, of, mode)
	u32 name, of, mode;
{
	u32 a[3];

	a[0] = name;
	a[1] = of;
	a[2] = mode;
	return k_OpenFile(a);
}

static u32
k_GetTempFileName(a)
	u32 *a;
{
	static int seq;
	char *out = gptr(a[3]), *pre = STR(a[1]);
	int u = LO16(a[2]) ? LO16(a[2]) : (getpid() + ++seq) & 0xffff;
	int d = (a[0] & 0xff) && !(a[0] & 0x80) ? (a[0] & 0xff) : 'C';

	if (!out)
		return 0;
	sprintf(out, "%c:\\TEMP\\~%.3s%04X.TMP", d & 0x5f, pre, u);
	if (!LO16(a[2])) {
		int h = dos_creat(out, 0);

		if (h >= 0)
			dos_close(h);
	}
	return u;
}

static u32 k_GetTempDrive(a) u32 *a; { return 'C' | 0x80 << 8; }

static u32
k_GetDriveType(a)
	u32 *a;
{
	int d = a[0];

	if (d < 0 || d >= 26 || !drive_root[d])
		return 0;
	/* removable (A:, B:), fixed, or remote as Wabi's configuration calls it */
	return d < 2 ? 2 : wabi_remote(d) ? 4 : 3;
}

static u32
k_GetWindowsDirectory(a)
	u32 *a;
{
	char *d = gptr(a[0]);

	if (d && a[1] > strlen(windir))
		strcpy(d, windir);
	return strlen(windir);
}

static u32
k_GetSystemDirectory(a)
	u32 *a;
{
	char *d = gptr(a[0]);

	if (d && a[1] > strlen(sysdir))
		strcpy(d, sysdir);
	return strlen(sysdir);
}

static u32 k_SetHandleCount(a) u32 *a; { return a[0] < 20 ? 20 : a[0]; }

static u32
k_SetErrorMode(a)
	u32 *a;
{
	static u16 mode;
	u16 o = mode;

	mode = a[0];
	return o;
}

static u32 k_GetLastDiskChange(a) u32 *a; { return 0; }

/* DOS3Call: INT 21h with the caller's registers */
static u32
k_DOS3Call(a)
	u32 *a;
{
	dos_int21(cpu);
	return 0;
}

static u32
k_NetBIOSCall(a)
	u32 *a;
{
	cpu->r[R_AX] = (cpu->r[R_AX] & ~0xff) | 0xfb;
	return 0;
}

/* ---- Catch and Throw (register entries) ---- */

static u32
k_Catch(a)
	u32 *a;
{
	u32 ss = cpu->s[S_SS].base, sp = cpu->r[R_SP] & 0xffff;
	u32 buf = lin(FPSEL(a[0]), FPOFF(a[0]));

	if (!buf)
		return 0;
	PW(buf, GW(ss + sp));			/* IP */
	PW(buf + 2, GW(ss + sp + 2));		/* CS */
	PW(buf + 4, sp + 8);			/* SP after the return */
	PW(buf + 6, cpu->r[R_BP]);
	PW(buf + 8, cpu->r[R_SI]);
	PW(buf + 10, cpu->r[R_DI]);
	PW(buf + 12, cpu->s[S_DS].sel);
	PW(buf + 14, 0);
	PW(buf + 16, cpu->s[S_SS].sel);
	cpu->r[R_AX] &= ~0xffff;
	return 0;
}

static u32
k_Throw(a)
	u32 *a;
{
	u32 buf = lin(FPSEL(a[0]), FPOFF(a[0]));

	if (!buf)
		w16_fatal("Throw to a bad CATCHBUF");
	x86_loadseg(cpu, S_SS, GW(buf + 16));
	cpu->r[R_SP] = GW(buf + 4);
	cpu->r[R_BP] = GW(buf + 6);
	cpu->r[R_SI] = GW(buf + 8);
	cpu->r[R_DI] = GW(buf + 10);
	x86_loadseg(cpu, S_DS, GW(buf + 12));
	x86_loadseg(cpu, S_CS, GW(buf + 2));
	cpu->eip = GW(buf);
	cpu->r[R_AX] = LO16(a[1]);
	api_jumped = 1;
	return 0;
}

/* ---- errors and debugging ---- */

static u32
k_FatalExit(a)
	u32 *a;
{
	w16_log("startwin: FatalExit(%d)\n", (short)a[0]);
	w16_exit(255);
	return 0;
}

static u32
k_FatalAppExit(a)
	u32 *a;
{
	extern int user_messagebox();

	user_messagebox(0, STR(a[1]), "Application Error", 0x10);
	w16_exit(255);
	return 0;
}

static u32
k_OutputDebugString(a)
	u32 *a;
{
	if (w16_debug)
		w16_log("%s", STR(a[0]));
	return 0;
}

static u32 k_DebugBreak(a) u32 *a; { return 0; }
static u32 k_nop(a) u32 *a; { return 0; }
static u32 k_one(a) u32 *a; { return 1; }

static u32
k_ExitProcess(a)
	u32 *a;
{
	w16_exit(a[0]);
	return 0;
}

/* ---- strings ---- */

static u32
k_lstrcpy(a)
	u32 *a;
{
	char *d = gptr(a[0]), *s = gptr(a[1]);

	if (!d)
		return 0;
	if (s)
		memmove(d, s, strlen(s) + 1);
	else
		*d = 0;
	return a[0];
}

static u32
k_lstrcpyn(a)
	u32 *a;
{
	char *d = gptr(a[0]), *s = STR(a[1]);
	int n = (short)a[2], l;

	if (!d || n <= 0)
		return a[0];
	l = strlen(s);
	if (l > n - 1)
		l = n - 1;
	memmove(d, s, l);
	d[l] = 0;
	return a[0];
}

static u32
k_lstrcat(a)
	u32 *a;
{
	char *d = gptr(a[0]), *s = STR(a[1]);

	if (d)
		memmove(d + strlen(d), s, strlen(s) + 1);
	return a[0];
}

static u32
k_lstrcatn(a)
	u32 *a;
{
	char *d = gptr(a[0]), *s = STR(a[1]);
	int n = (short)a[2], l;

	if (!d)
		return a[0];
	l = strlen(d);
	if (n - l - 1 > 0)
		strncat(d, s, n - l - 1);
	return a[0];
}

static u32 k_lstrlen(a) u32 *a; { return gstrlen(a[0]); }

/* ---- atoms: one table for the local and global ones ---- */

#define	NATOM	1024
static char *atoms[NATOM];
static u16 atomref[NATOM];

static u32
atom_find(s)
	char *s;
{
	int i;

	for (i = 0; i < NATOM; i++)
		if (atoms[i] && w16_stricmp(atoms[i], s) == 0)
			return 0xc000 + i;
	return 0;
}

u32
atom_add(p)
	u32 p;
{
	char *s;
	int i;
	u32 r;

	if (ISINT(p))
		return FPOFF(p);
	s = STR(p);
	if (s[0] == '#')
		return atoi(s + 1);
	if ((r = atom_find(s)) != 0) {
		atomref[r - 0xc000]++;
		return r;
	}
	for (i = 0; i < NATOM; i++)
		if (!atoms[i]) {
			atoms[i] = strdup(s);
			atomref[i] = 1;
			return 0xc000 + i;
		}
	return 0;
}

char *
atom_name(at)
	u32 at;
{
	static char num[8];

	if (at >= 0xc000 && at < 0xc000 + NATOM && atoms[at - 0xc000])
		return atoms[at - 0xc000];
	if (at && at < 0xc000) {
		sprintf(num, "#%u", at);
		return num;
	}
	return 0;
}

static u32 k_InitAtomTable(a) u32 *a; { return 1; }
static u32 k_FileCDR(a) u32 *a; { return 1; }	/* file change notices: taken, none are sent */

static u32
k_LocalHandleDelta(a)
	u32 *a;
{
	static int delta = 32;

	if (a[0] & 0xffff)
		delta = a[0] & 0xffff;
	return delta;
}
static u32 k_AddAtom(a) u32 *a; { return atom_add(a[0]); }

static u32
k_FindAtom(a)
	u32 *a;
{
	char *s;

	if (ISINT(a[0]))
		return FPOFF(a[0]);
	s = STR(a[0]);
	return s[0] == '#' ? atoi(s + 1) : atom_find(s);
}

static u32
k_DeleteAtom(a)
	u32 *a;
{
	u32 at = LO16(a[0]);

	if (at >= 0xc000 && at < 0xc000 + NATOM && atoms[at - 0xc000] && --atomref[at - 0xc000] == 0) {
		free(atoms[at - 0xc000]);
		atoms[at - 0xc000] = 0;
	}
	return 0;
}

static u32
k_GetAtomName(a)
	u32 *a;
{
	char *n = atom_name(LO16(a[0])), *d = gptr(a[1]);
	int max = (short)a[2];

	if (!n || !d || max <= 0)
		return 0;
	strncpy(d, n, max - 1);
	d[max - 1] = 0;
	return strlen(d);
}

/* ---- profiles ---- */

static u32
k_GetProfileInt(a)
	u32 *a;
{
	char buf[32];

	if (!profile_get((char *)0, STR(a[0]), STR(a[1]), "", buf, sizeof buf) || !buf[0])
		return LO16(a[2]);
	return (u32)atoi(buf) & 0xffff;
}

static u32
k_GetPrivateProfileInt(a)
	u32 *a;
{
	char buf[32];

	if (!profile_get(STR(a[3]), STR(a[0]), STR(a[1]), "", buf, sizeof buf) || !buf[0])
		return LO16(a[2]);
	return (u32)atoi(buf) & 0xffff;
}

static u32
k_GetProfileString(a)
	u32 *a;
{
	char *out = gptr(a[3]);

	if (!out)
		return 0;
	return profile_get((char *)0, gptr(a[0]), gptr(a[1]), STR(a[2]), out, a[4]);
}

static u32
k_GetPrivateProfileString(a)
	u32 *a;
{
	char *out = gptr(a[3]);

	if (!out)
		return 0;
	return profile_get(STR(a[5]), gptr(a[0]), gptr(a[1]), STR(a[2]), out, a[4]);
}

static u32
k_WriteProfileString(a)
	u32 *a;
{
	return profile_put((char *)0, STR(a[0]), gptr(a[1]), gptr(a[2]));
}

static u32
k_WritePrivateProfileString(a)
	u32 *a;
{
	return profile_put(STR(a[3]), STR(a[0]), gptr(a[1]), gptr(a[2]));
}

/* ---- programs ---- */

/*
 * A program started: its module (a copy of its own when the program is
 * already running: each instance its own segments), its libraries'
 * initialization, then a task; the new one runs at once, as Windows has
 * WinExec yield to it.  The instance handle, or an error below 32.
 */
static u32
startprog(path, args, show)
	char *path, *args;
	int show;
{
	extern void initdeps();
	struct module *m, *old;
	struct task *t;
	char dos[300], name[300], *p;
	int err;

	strncpy(name, path, sizeof name - 5);
	name[sizeof name - 5] = 0;
	p = strrchr(name, '\\');
	if (!strchr(p ? p : name, '.'))
		strcat(name, ".EXE");
	w16_upper(name);
	if (dos_fullpath(name, dos) != 0)
		strcpy(dos, name);
	/* a bare name: the current directory, then Windows', then SYSTEM, as LoadModule and WinExec look (MMTASK.TSK) */
	if (!strchr(name, '\\') && !strchr(name, ':')) {
		extern char windir[], sysdir[];
		char try[300], host[1024];
		char *dirs[2];
		int i;

		dirs[0] = windir;
		dirs[1] = sysdir;
		for (i = -1; i < 2; i++) {
			if (i < 0)
				strcpy(try, dos);
			else
				sprintf(try, "%s\\%s", dirs[i], name);
			if (dos_hostpath(try, host, sizeof host, 0) == 0 && access(host, 0) == 0) {
				if (dos_fullpath(try, dos) != 0)
					strcpy(dos, try);
				break;
			}
		}
	}
	old = mod_find_path(dos);
	m = old && !old->m_dll ? mod_loadcopy(dos, &err) : mod_load(dos, &err);
	if (!m)
		return err ? err : 2;
	if (m->m_dll) {
		mod_free(m);
		return 11;
	}
	initdeps(m, 0);
	if ((t = task_new(m, args, show)) == 0) {
		mod_free(m);
		return 8;
	}
	if (w16_debug)
		w16_log("startwin: %s (%s) started: \"%s\"\n", m->m_name, m->m_path, t->t_cmdline);
	/* as Windows 3.1: back once the new program asks for its first message (OLE's servers answer by then) */
	{
		int i;
		u16 hinst = m->m_hinst;

		for (i = 0; i < 2000 && curtask && task_alive(t) && !t->t_ready; i++)
			task_yield();
		return hinst;
	}
}

/* WinExec for the host's own use (WinHelp's viewer) */
u32
kernel_winexec(line, show)
	char *line;
	int show;
{
	char buf[300], *p, *args;

	strncpy(buf, line, sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	for (args = buf; *args && *args != ' '; args++)
		;
	if (*args)
		*args++ = 0;
	p = buf;
	return startprog(p, args, show);
}

static u32
k_WinExec(a)
	u32 *a;
{
	char line[300], *p, *args;

	strncpy(line, STR(a[0]), sizeof line - 1);
	line[sizeof line - 1] = 0;
	for (p = line; *p == ' '; p++)
		;
	for (args = p; *args && *args != ' '; args++)
		;
	if (*args)
		*args++ = 0;
	while (*args == ' ')
		args++;
	return startprog(p, args, (short)a[1]);
}

/* LoadModule(name, params): params is the environment, the command tail (a length byte first), the show */
static u32
k_LoadModule(a)
	u32 *a;
{
	u32 pb = lin(FPSEL(a[1]), FPOFF(a[1])), tail, sh;
	char args[130], *p;
	int show = 1, n;	/* SW_SHOWNORMAL */

	args[0] = 0;
	if (pb) {
		if ((tail = lin(FPSEL(GL(pb + 2)), FPOFF(GL(pb + 2)))) != 0) {
			n = M[tail] > 126 ? 126 : M[tail];
			/*
			 * text going on past the count to its NUL or CR is the tail
			 * (OLECLI starts a server with " -Embedding file" as a C
			 * string: its "count" is the space)
			 */
			{
				int k = n;

				while (k < 126 && M[tail + 1 + k] >= 0x20)
					k++;
				if (k > n && (M[tail + 1 + k] == 0 || M[tail + 1 + k] == 0x0d))
					n = k;
			}
			memcpy(args, M + tail + 1, n);
			args[n] = 0;
			/* not text (a NUL in it): passed to the PSP as it is */
			if ((int)strlen(args) < n) {
				memcpy(rawtail, args, n);
				rawlen = n;
			}
			if ((p = strchr(args, '\r')) != 0)
				*p = 0;
		}
		if ((sh = lin(FPSEL(GL(pb + 6)), FPOFF(GL(pb + 6)))) != 0)
			show = GW(sh + 2);
	}
	n = startprog(STR(a[0]), args[0] == ' ' ? args + 1 : args, show);
	rawlen = -1;
	return n;
}

static u32 k_GetCodeHandle(a) u32 *a; { return FPSEL(a[0]); }

static u32
k_GetHeapSpaces(a)
	u32 *a;
{
	return FP(0x10000 - 1, 0x8000);
}

/* ---- the table ---- */

int api_jumped;

struct impl k_impl[] = {
	{ "KERNEL", "InitTask", k_InitTask },
	{ "KERNEL", "WaitEvent", k_WaitEvent },
	{ "KERNEL", "Yield", k_Yield },
	{ "KERNEL", "OldYield", k_Yield },
	{ "KERNEL", "DirectedYield", k_Yield },
	{ "KERNEL", "GetVersion", k_GetVersion },
	{ "KERNEL", "GetWinFlags", k_GetWinFlags },
	{ "KERNEL", "GetCurrentTask", k_GetCurrentTask },
	{ "KERNEL", "GetCurrentPDB", k_GetCurrentPDB },
	{ "KERNEL", "GetNumTasks", k_GetNumTasks },
	{ "KERNEL", "IsTaskLocked", k_IsTaskLocked },
	{ "KERNEL", "IsTask", k_IsTask },
	{ "KERNEL", "GetTaskQueue", k_GetTaskQueue },
	{ "KERNEL", "SetTaskQueue", k_SetTaskQueue },
	{ "KERNEL", "GetDOSEnvironment", k_GetDOSEnvironment },
	{ "KERNEL", "GetExePtr", k_GetExePtr },
	{ "KERNEL", "GetModuleHandle", k_GetModuleHandle },
	{ "KERNEL", "GetModuleUsage", k_GetModuleUsage },
	{ "KERNEL", "GetModuleFileName", k_GetModuleFileName },
	{ "KERNEL", "GetModuleName", k_GetModuleName },
	{ "KERNEL", "GetProcAddress", k_GetProcAddress },
	{ "KERNEL", "LoadLibrary", k_LoadLibrary },
	{ "KERNEL", "FreeLibrary", k_FreeLibrary },
	{ "KERNEL", "FreeModule", k_FreeModule },
	{ "KERNEL", "MakeProcInstance", k_MakeProcInstance },
	{ "KERNEL", "FreeProcInstance", k_FreeProcInstance },
	{ "KERNEL", "GetInstanceData", k_GetInstanceData },
	{ "KERNEL", "GetExpWinVer", k_GetExpWinVer },
	{ "KERNEL", "GlobalAlloc", k_GlobalAlloc },
	{ "KERNEL", "GlobalReAlloc", k_GlobalReAlloc },
	{ "KERNEL", "GlobalFree", k_GlobalFree },
	{ "KERNEL", "GlobalLock", k_GlobalLock },
	{ "KERNEL", "GlobalUnlock", k_GlobalUnlock },
	{ "KERNEL", "GlobalSize", k_GlobalSize },
	{ "KERNEL", "GlobalHandle", k_GlobalHandle },
	{ "KERNEL", "GlobalHandleNoRIP", k_GlobalHandle },
	{ "KERNEL", "GlobalFlags", k_GlobalFlags },
	{ "KERNEL", "GlobalCompact", k_GlobalCompact },
	{ "KERNEL", "GetFreeSpace", k_GetFreeSpace },
	{ "KERNEL", "GlobalFix", k_GlobalFix },
	{ "KERNEL", "GlobalUnfix", k_GlobalFix },
	{ "KERNEL", "LockSegment", k_LockSegment },
	{ "KERNEL", "UnlockSegment", k_UnlockSegment },
	{ "KERNEL", "GlobalWire", k_GlobalWire },
	{ "KERNEL", "GlobalUnWire", k_GlobalUnWire },
	{ "KERNEL", "GlobalPageLock", k_GlobalPageLock },
	{ "KERNEL", "GlobalPageUnlock", k_GlobalPageLock },
	{ "KERNEL", "GlobalLRUOldest", k_GlobalLRU },
	{ "KERNEL", "GlobalLRUNewest", k_GlobalLRU },
	{ "KERNEL", "GlobalNotify", k_nop },
	{ "KERNEL", "GlobalDOSAlloc", k_GlobalDOSAlloc },
	{ "KERNEL", "GlobalDOSFree", k_GlobalFree },
	{ "KERNEL", "AllocSelector", k_AllocSelector },
	{ "KERNEL", "AllocSelectorArray", k_AllocSelectorArray },
	{ "KERNEL", "FreeSelector", k_FreeSelector },
	{ "KERNEL", "AllocCStoDSAlias", k_AllocCStoDSAlias },
	{ "KERNEL", "AllocDStoCSAlias", k_AllocDStoCSAlias },
	{ "KERNEL", "AllocAlias", k_AllocCStoDSAlias },
	{ "KERNEL", "PrestoChangoSelector", k_PrestoChangoSelector },
	{ "KERNEL", "GetSelectorBase", k_GetSelectorBase },
	{ "KERNEL", "SetSelectorBase", k_SetSelectorBase },
	{ "KERNEL", "GetSelectorLimit", k_GetSelectorLimit },
	{ "KERNEL", "SetSelectorLimit", k_SetSelectorLimit },
	{ "KERNEL", "SelectorAccessRights", k_SelectorAccessRights },
	{ "KERNEL", "LongPtrAdd", k_LongPtrAdd },
	{ "KERNEL", "hmemcpy", k_hmemcpy },
	{ "KERNEL", "IsBadReadPtr", k_IsBadPtr },
	{ "KERNEL", "IsBadWritePtr", k_IsBadPtr },
	{ "KERNEL", "IsBadHugeReadPtr", k_IsBadPtr },
	{ "KERNEL", "IsBadHugeWritePtr", k_IsBadPtr },
	{ "KERNEL", "IsBadCodePtr", k_IsBadStringPtr },
	{ "KERNEL", "IsBadStringPtr", k_IsBadStringPtr },
	{ "KERNEL", "LocalInit", k_LocalInit },
	{ "KERNEL", "LocalAlloc", k_LocalAlloc },
	{ "KERNEL", "LocalReAlloc", k_LocalReAlloc },
	{ "KERNEL", "LocalFree", k_LocalFree },
	{ "KERNEL", "LocalLock", k_LocalLock },
	{ "KERNEL", "LocalUnlock", k_LocalUnlock },
	{ "KERNEL", "LocalSize", k_LocalSize },
	{ "KERNEL", "LocalHandle", k_LocalHandle },
	{ "KERNEL", "LocalFlags", k_LocalFlags },
	{ "KERNEL", "LocalCompact", k_LocalCompact },
	{ "KERNEL", "LocalShrink", k_LocalShrink },
	{ "KERNEL", "LocalNotify", k_nop },
	{ "KERNEL", "FindResource", k_FindResource },
	{ "KERNEL", "LoadResource", k_LoadResource },
	{ "KERNEL", "LockResource", k_LockResource },
	{ "KERNEL", "FreeResource", k_FreeResource },
	{ "KERNEL", "SizeofResource", k_SizeofResource },
	{ "KERNEL", "AccessResource", k_AccessResource },
	{ "KERNEL", "AllocResource", k_AllocResource },
	{ "KERNEL", "DirectResAlloc", k_DirectResAlloc },
	{ "KERNEL", "SetResourceHandler", k_SetResourceHandler },
	{ "KERNEL", "_lopen", k_lopen },
	{ "KERNEL", "_lcreat", k_lcreat },
	{ "KERNEL", "_lclose", k_lclose },
	{ "KERNEL", "_lread", k_lread },
	{ "KERNEL", "_lwrite", k_lwrite },
	{ "KERNEL", "_llseek", k_llseek },
	{ "KERNEL", "_hread", k_hread },
	{ "KERNEL", "_hwrite", k_hwrite },
	{ "KERNEL", "OpenFile", k_OpenFile },
	{ "KERNEL", "GetTempFileName", k_GetTempFileName },
	{ "KERNEL", "GetTempDrive", k_GetTempDrive },
	{ "KERNEL", "GetDriveType", k_GetDriveType },
	{ "KERNEL", "GetWindowsDirectory", k_GetWindowsDirectory },
	{ "KERNEL", "GetSystemDirectory", k_GetSystemDirectory },
	{ "KERNEL", "SetHandleCount", k_SetHandleCount },
	{ "KERNEL", "SetErrorMode", k_SetErrorMode },
	{ "KERNEL", "GetLastDiskChange", k_GetLastDiskChange },
	{ "KERNEL", "DOS3Call", k_DOS3Call },
	{ "KERNEL", "NetBIOSCall", k_NetBIOSCall },
	{ "KERNEL", "Catch", k_Catch },
	{ "KERNEL", "Throw", k_Throw },
	{ "KERNEL", "FatalExit", k_FatalExit },
	{ "KERNEL", "FatalAppExit", k_FatalAppExit },
	{ "KERNEL", "OutputDebugString", k_OutputDebugString },
	{ "KERNEL", "DebugBreak", k_DebugBreak },
	{ "KERNEL", "ExitProcess", k_ExitProcess },
	{ "KERNEL", "lstrcpy", k_lstrcpy },
	{ "KERNEL", "lstrcpyn", k_lstrcpyn },
	{ "KERNEL", "lstrcat", k_lstrcat },
	{ "KERNEL", "lstrcatn", k_lstrcatn },
	{ "KERNEL", "lstrlen", k_lstrlen },
	{ "KERNEL", "InitAtomTable", k_InitAtomTable },
	{ "KERNEL", "AddAtom", k_AddAtom },
	{ "KERNEL", "FindAtom", k_FindAtom },
	{ "KERNEL", "DeleteAtom", k_DeleteAtom },
	{ "KERNEL", "GetAtomName", k_GetAtomName },
	/* the global atoms are USER's in 3.1; one table serves both here */
	{ "USER", "GlobalAddAtom", k_AddAtom },
	{ "USER", "GlobalFindAtom", k_FindAtom },
	{ "USER", "GlobalDeleteAtom", k_DeleteAtom },
	{ "USER", "GlobalGetAtomName", k_GetAtomName },
	{ "KERNEL", "LocalHandleDelta", k_LocalHandleDelta },
	{ "KERNEL", "FileCDR", k_FileCDR },
	{ "KERNEL", "GetProfileInt", k_GetProfileInt },
	{ "KERNEL", "GetPrivateProfileInt", k_GetPrivateProfileInt },
	{ "KERNEL", "GetProfileString", k_GetProfileString },
	{ "KERNEL", "GetPrivateProfileString", k_GetPrivateProfileString },
	{ "KERNEL", "WriteProfileString", k_WriteProfileString },
	{ "KERNEL", "WritePrivateProfileString", k_WritePrivateProfileString },
	{ "KERNEL", "WinExec", k_WinExec },
	{ "KERNEL", "LoadModule", k_LoadModule },
	{ "KERNEL", "GetCodeHandle", k_GetCodeHandle },
	{ "KERNEL", "GetHeapSpaces", k_GetHeapSpaces },
	{ "KERNEL", "SetPriority", k_nop },
	{ "KERNEL", "LockCurrentTask", k_nop },
	{ "KERNEL", "SetTaskSignalProc", k_nop },
	{ "KERNEL", "SetSwapAreaSize", k_nop },
	{ "KERNEL", "ValidateCodeSegments", k_nop },
	{ "KERNEL", "ValidateFreeSpaces", k_nop },
	{ "KERNEL", "DefineHandleTable", k_one },
	{ "KERNEL", "SetSigHandler", k_nop },
	{ "KERNEL", "IsDBCSLeadByte", k_nop },
	{ "KERNEL", "LimitEMSPages", k_nop },
	{ "KERNEL", "GetAppCompatFlags", k_nop },
	{ "KERNEL", "PostEvent", k_PostEvent },
	{ "KERNEL", "UndefDynLink", k_nop },
	{ 0 }
};
