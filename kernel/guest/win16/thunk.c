/*
 * thunk.c -- where x86 code meets native code.
 *
 * Every export of our system modules is a four-byte thunk, 0F FF n,
 * in one code segment; the CPU hands n to thunk_dispatch, which reads
 * the arguments the entry's signature (apitab) gives, calls our handler,
 * puts its result in AX (DX:AX for a long), and returns far, popping
 * the arguments of a Pascal entry.  The registers Windows preserves are
 * put back whatever the handler did.  Thunk 0 ends an x86_call.
 *
 * Native code calls x86 code with cb_begin, cb_push16/32 and cb_call,
 * which loads AX and DS with the callee's DGROUP the way an instance
 * thunk would.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "apitab.h"

#define	NTHUNK	16384		/* 64K of four-byte thunks */
#define	FIRSTDYN 12288		/* from here: internal entries made at run time */

struct thunk {
	struct module *t_mod;
	struct apient *t_ae;
	apifn	t_fn;
	int	t_warned;
};

u16 thunksel;
u32 api_varargs;
u16 api_callerds;
int w16_strict;

static struct thunk thunks[NTHUNK];
static int nthunk = 1, ndyn = FIRSTDYN;
static u16 instsel;		/* instance thunks */
static int ninst;
static u16 varsel;		/* the system modules' variables */
static int nvar;

static struct impl *impls[] = { k_impl, u_impl, g_impl, mn_impl, dl_impl, ct_impl, sb_impl, md_impl, cu_impl, dv_impl, mm_impl, ws_impl, o_impl, 0 };

static apifn
findimpl(mod, name)
	char *mod, *name;
{
	struct impl **ip, *i;

	for (ip = impls; *ip; ip++)
		for (i = *ip; i->im_mod; i++)
			if (strcmp(i->im_mod, mod) == 0 && strcmp(i->im_name, name) == 0)
				return i->im_fn;
	return 0;
}

void
thunk_init()
{
	u32 b;
	int i;

	thunksel = g_alloc(GMEM_ZEROINIT, NTHUNK * 4, 0);
	b = sel_base(thunksel);
	for (i = 0; i < NTHUNK; i++) {
		M[b + 4 * i] = 0x0f;
		M[b + 4 * i + 1] = 0xff;
		M[b + 4 * i + 2] = i;
		M[b + 4 * i + 3] = i >> 8;
	}
	g_block(thunksel)->gb_code = 1;
	LDT[SELIX(thunksel)].d_acc = D_P | D_S | D_CODE | D_R | D_A;
	x86_retthunk = 0;
	x86_setret(thunksel, 0);
	instsel = g_alloc(GMEM_ZEROINIT, 0x10000, 0);
	g_block(instsel)->gb_code = 1;
	LDT[SELIX(instsel)].d_acc = D_P | D_S | D_CODE | D_R | D_A;
	varsel = g_alloc(GMEM_ZEROINIT, 0x1000, 0);
}

/* the far pointer standing for a native module's export */
u32
thunk_native(m, ord)
	struct module *m;
	int ord;
{
	struct apient *e;
	u32 v, a;
	char *p;
	int i;

	if (!m->m_thunk) {
		if (nthunk + m->m_nent + 1 > FIRSTDYN)
			w16_fatal("out of thunks");
		m->m_thunk = nthunk;
		nthunk += m->m_nent + 1;
	}
	for (e = m->m_api->am_ent; e->ae_name; e++)
		if (e->ae_ord == ord)
			break;
	if (!e->ae_name || ord < 1 || ord > m->m_nent)
		return 0;
	switch (e->ae_kind) {
	case 'e':
		v = atoi(e->ae_args);
		if (strcmp(e->ae_name, "__0040H") == 0 || strcmp(e->ae_name, "__0000H") == 0 ||
		    strcmp(e->ae_name, "__A000H") == 0 || strcmp(e->ae_name, "__B000H") == 0 ||
		    strcmp(e->ae_name, "__B800H") == 0 || strcmp(e->ae_name, "__C000H") == 0 ||
		    strcmp(e->ae_name, "__D000H") == 0 || strcmp(e->ae_name, "__E000H") == 0 ||
		    strcmp(e->ae_name, "__F000H") == 0 || strcmp(e->ae_name, "__ROMBIOS") == 0)
			v = kernel_lowsel(e->ae_name);
		else if (strcmp(e->ae_name, "__WINFLAGS") == 0)
			v = kernel_winflags();
		return FP(v, v);
	case 'V':
		/* the values, once, in the variables' segment */
		i = m->m_thunk + ord;
		if (!thunks[i].t_ae) {
			thunks[i].t_mod = m;
			thunks[i].t_ae = e;
			thunks[i].t_warned = nvar;
			a = sel_base(varsel) + nvar;
			for (p = e->ae_args; *p; ) {
				PL(a, strtoul(p, &p, 0));
				a += 4;
				nvar += 4;
				while (*p == ' ')
					p++;
			}
		}
		return FP(varsel, thunks[i].t_warned);
	}
	i = m->m_thunk + ord;
	if (!thunks[i].t_ae) {
		thunks[i].t_mod = m;
		thunks[i].t_ae = e;
		thunks[i].t_fn = findimpl(m->m_name, e->ae_name);
	}
	return FP(thunksel, 4 * i);
}

/* an internal native entry (a window procedure of ours, a callback) as a far pointer */
u32
thunk_internal(fn, args, ret, name)
	apifn fn;
	char *args, *name;
	int ret;
{
	struct apient *e;
	int i;

	for (i = FIRSTDYN; i < ndyn; i++)
		if (thunks[i].t_fn == fn)
			return FP(thunksel, 4 * i);
	if (ndyn >= NTHUNK)
		w16_fatal("out of thunks");
	e = (struct apient *)calloc(1, sizeof *e);
	e->ae_name = name;
	e->ae_kind = 'p';
	e->ae_args = args;
	e->ae_ret = ret;
	thunks[ndyn].t_ae = e;
	thunks[ndyn].t_fn = fn;
	return FP(thunksel, 4 * ndyn++);
}

int
thunk_isnative(p, fnp)
	u32 p;
	apifn *fnp;
{
	int n;

	if (FPSEL(p) != thunksel || (FPOFF(p) & 3))
		return 0;
	n = FPOFF(p) >> 2;
	if (!thunks[n].t_ae)
		return 0;
	if (fnp)
		*fnp = thunks[n].t_fn;
	return n;
}

/* MakeProcInstance: mov ax,ds; jmp far proc */
u32
thunk_procinst(proc, ds)
	u32 proc;
	int ds;
{
	u32 b = sel_base(instsel);
	int i;

	if (!ds || thunk_isnative(proc, (apifn *)0))
		return proc;
	for (i = 0; i < ninst; i++)
		if (GW(b + 8 * i + 1) == ds && GL(b + 8 * i + 4) == proc && M[b + 8 * i] == 0xb8)
			return FP(instsel, 8 * i);
	for (i = 0; i < ninst; i++)
		if (M[b + 8 * i] == 0)
			break;
	if (i == ninst) {
		if (ninst >= 0x10000 / 8)
			return proc;
		ninst++;
	}
	M[b + 8 * i] = 0xb8;
	PW(b + 8 * i + 1, ds);
	M[b + 8 * i + 3] = 0xea;
	PL(b + 8 * i + 4, proc);
	return FP(instsel, 8 * i);
}

void
thunk_freeinst(p)
	u32 p;
{
	if (FPSEL(p) == instsel && FPOFF(p) % 8 == 0)
		memset(M + sel_base(instsel) + FPOFF(p), 0, 8);
}

/* the code an instance thunk leads to, and its DS */
static u32
throughinst(p, dsp)
	u32 p;
	u16 *dsp;
{
	u32 b = sel_base(instsel) + FPOFF(p);

	if (FPSEL(p) != instsel || M[b] != 0xb8)
		return p;
	*dsp = GW(b + 1);
	return GL(b + 4);
}

/* ---- dispatch ---- */

struct regs {
	u32	r[8];
	u16	s[6];
};

static void
saveregs(rp)
	struct regs *rp;
{
	int i;

	memcpy(rp->r, cpu->r, sizeof rp->r);
	for (i = 0; i < 6; i++)
		rp->s[i] = cpu->s[i].sel;
}

static void
restregs(rp, keep)
	struct regs *rp;
	int keep;		/* registers set by the call: 1 AX, 2 DX */
{
	u32 ax = cpu->r[R_AX], dx = cpu->r[R_DX];
	int i;

	memcpy(cpu->r, rp->r, sizeof rp->r);
	if (keep & 1)
		cpu->r[R_AX] = ax;
	if (keep & 2)
		cpu->r[R_DX] = dx;
	for (i = 0; i < 6; i++)
		if (i != S_CS && cpu->s[i].sel != rp->s[i])
			x86_loadseg(cpu, i, rp->s[i]);
}

static int
argsize(c)
	int c;
{
	switch (c) {
	case 'w':
	case 's':
		return 2;
	case 'q':
		return 8;
	}
	return 4;
}

int api_depth;
extern int api_jumped;

void
thunk_dispatch(c, n)
	struct x86 *c;
	int n;
{
	struct thunk *t = &thunks[n];
	struct apient *e = t->t_ae;
	struct regs save;
	u32 ss = c->s[S_SS].base, sp = c->r[R_SP] & 0xffff, a[24], r = 0, pos;
	u32 rip, rcs;
	int total = 0, i, na, pascal;
	char *p;

	if (!e)
		w16_fatal("call to an empty thunk %d", n);
	/* the drivers' interrupt-time work, between calls too: a program busy outside the message loop still plays */
	{
		static int calls;
		static u32 last;
		extern void mm_tick();

		if (++calls >= 64) {
			calls = 0;
			if (w16_ticks() - last >= 20) {
				last = w16_ticks();
				mm_tick();
				ss = c->s[S_SS].base;
				sp = c->r[R_SP] & 0xffff;
			}
		}
	}
	rip = GW(ss + sp);
	rcs = GW(ss + sp + 2);
	for (p = e->ae_args; *p; p++)
		total += argsize(*p);
	na = strlen(e->ae_args);
	pascal = e->ae_kind == 'p' || e->ae_kind == 'r';
	if (pascal) {
		pos = sp + 4 + total;
		for (i = 0; i < na && i < 24; i++) {
			pos -= argsize(e->ae_args[i]);
			switch (e->ae_args[i]) {
			case 'w': a[i] = GW(ss + ((pos) & 0xffff)); break;
			case 's': a[i] = (u32)(s32)(short)GW(ss + ((pos) & 0xffff)); break;
			default: a[i] = GL(ss + ((pos) & 0xffff)); break;
			}
		}
	} else {
		pos = sp + 4;
		for (i = 0; i < na && i < 24; i++) {
			switch (e->ae_args[i]) {
			case 'w': a[i] = GW(ss + pos); break;
			case 's': a[i] = (u32)(s32)(short)GW(ss + pos); break;
			default: a[i] = GL(ss + pos); break;
			}
			pos += argsize(e->ae_args[i]);
		}
		api_varargs = FP(c->s[S_SS].sel, pos);
	}
	if (w16_debug > 1)
		w16_log("call %s.%s from %04x:%04x\n", t->t_mod ? t->t_mod->m_name : "", e->ae_name, rcs, rip);
	if (w16_debug > 2 && getenv("W16_BT") && strcmp(getenv("W16_BT"), e->ae_name) == 0) {
		/* the callers, along the BP chain (odd BP: a far frame) */
		u32 bp = c->r[R_BP] & 0xffff, ssb = c->s[S_SS].base;
		int k;

		for (k = 0; k < 12 && bp; k++) {
			u16 nb = GW(ssb + (bp & ~1)), ip = GW(ssb + (bp & ~1) + 2), cs = GW(ssb + (bp & ~1) + 4);

			struct module *m = bp & 1 ? mod_byhandle(cs) : 0;
			int i, seg = 0;

			for (i = 1; m && i <= m->m_nseg; i++)
				if (m->m_seg[i].ns_sel == cs)
					seg = i;
			w16_log("  frame %04x: ret %04x:%04x %s seg %d\n", bp, bp & 1 ? cs : 0, ip, m ? m->m_name : "", seg);
			bp = nb;
		}
	}
	saveregs(&save);
	api_callerds = c->s[S_DS].sel;
	api_jumped = 0;
	api_depth++;
	if (t->t_fn)
		r = (*t->t_fn)(a);
	else if (e->ae_kind == 's') {
		w16_fatal("%s.%s (ordinal %d) is not there; its arguments are unknown",
		    t->t_mod ? t->t_mod->m_name : "?", e->ae_name, e->ae_ord);
	} else {
		if (!t->t_warned++)
			w16_log("not done: %s.%s\n", t->t_mod ? t->t_mod->m_name : "?", e->ae_name);
		if (w16_strict)
			w16_fatal("%s.%s is not done", t->t_mod ? t->t_mod->m_name : "?", e->ae_name);
	}
	api_depth--;
	{
		static u32 wsel, woff, wval, armed;
		char *e2;

		if (!armed && (e2 = getenv("W16_WATCH")) != 0) {
			sscanf(e2, "%x:%x", &wsel, &woff);
			armed = 1;
		}
		if (armed == 1 && g_block(wsel)) {
			wval = GL(sel_base(wsel) + woff);
			armed = 2;
		} else if (armed == 2 && GL(sel_base(wsel) + woff) != wval) {
			w16_log("WATCH %x:%x changed %08lx -> %08lx in %s.%s base %lx\n", wsel, woff, (long)wval,
			    (long)GL(sel_base(wsel) + woff), t->t_mod ? t->t_mod->m_name : "", e->ae_name, (long)sel_base(wsel));
			wval = GL(sel_base(wsel) + woff);
		}
	}
	if (w16_debug > 2) {
		char args[200];
		int k, n = 0, m = e->ae_args ? strlen(e->ae_args) : 4;

		for (k = 0; k < m && k < 12; k++)
			n += sprintf(args + n, k ? " %lx" : "%lx", (long)a[k]);
		args[n] = 0;
		w16_log("  %s.%s(%s) = %lx\n", t->t_mod ? t->t_mod->m_name : "", e->ae_name, args, (long)r);
	}
	if (e->ae_kind == 'r') {
		/* register entries leave the registers as they set them; Throw went elsewhere */
		if (api_jumped)
			return;
		sp = c->r[R_SP] & 0xffff;
		c->eip = rip;
		x86_loadseg(c, S_CS, rcs);
		c->r[R_SP] = (c->r[R_SP] & ~0xffff) | ((sp + 4 + total) & 0xffff);
		return;
	}
	restregs(&save, 0);
	c->r[R_AX] = (c->r[R_AX] & ~0xffff) | (r & 0xffff);
	if (e->ae_ret == 'l')
		c->r[R_DX] = (c->r[R_DX] & ~0xffff) | (r >> 16);
	c->eip = rip;
	if (x86_loadseg(c, S_CS, rcs))
		w16_fatal("%s.%s returns to a bad code selector %04x", t->t_mod ? t->t_mod->m_name : "",
		    e->ae_name, rcs);
	c->r[R_SP] = (c->r[R_SP] & ~0xffff) | ((sp + 4 + (pascal ? total : 0)) & 0xffff);
}

/* ---- calling x86 code ---- */

#define	MAXCB	64
static struct regs *cbsave;	/* the task's own (thunk_ctx*) */
static int ncb;

/* each task's callback stack and the dispatcher's state, kept over task switches */
struct thctx {
	struct regs *cbsave;
	int	ncb;
	u32	varargs;
	u16	callerds;
	int	jumped, depth;
};

int
thunk_ctxsize()
{
	return sizeof(struct thctx);
}

void
thunk_ctxnew(p)
	char *p;
{
	struct thctx *x = (struct thctx *)p;

	memset(p, 0, sizeof *x);
	x->cbsave = (struct regs *)calloc(MAXCB, sizeof(struct regs));
}

void
thunk_ctxsave(p)
	char *p;
{
	extern int api_jumped;
	struct thctx *x = (struct thctx *)p;

	x->cbsave = cbsave;
	x->ncb = ncb;
	x->varargs = api_varargs;
	x->callerds = api_callerds;
	x->jumped = api_jumped;
	x->depth = api_depth;
}

void
thunk_ctxload(p)
	char *p;
{
	extern int api_jumped;
	struct thctx *x = (struct thctx *)p;

	cbsave = x->cbsave;
	ncb = x->ncb;
	api_varargs = x->varargs;
	api_callerds = x->callerds;
	api_jumped = x->jumped;
	api_depth = x->depth;
}

void
thunk_ctxfree(p)
	char *p;
{
	free((char *)((struct thctx *)p)->cbsave);
}

void
cb_begin()
{
	if (!cbsave)
		cbsave = (struct regs *)calloc(MAXCB, sizeof(struct regs));
	if (ncb >= MAXCB)
		w16_fatal("callbacks nested too deep");
	saveregs(&cbsave[ncb++]);
}

void
cb_push16(v)
	u32 v;
{
	x86_push16(cpu, v);
}

void
cb_push32(v)
	u32 v;
{
	x86_push16(cpu, v >> 16);
	x86_push16(cpu, v);
}

u16
cb_ds(p)
	u32 p;
{
	struct module *m = mod_byhandle(FPSEL(p));

	if (m && m->m_dgroup)
		return m->m_seg[m->m_dgroup].ns_sel;
	return 0;
}

/* call far p; the arguments pushed since cb_begin; DX:AX back */
u32
cb_call(p, ds)
	u32 p;
	int ds;
{
	u32 r;
	u16 ids = 0;

	if (ncb == 0)
		w16_fatal("cb_call without cb_begin");
	p = throughinst(p, &ids);
	if (ids)
		ds = ids;
	else if (!ds)
		ds = cb_ds(p);
	if (ds) {
		cpu->r[R_AX] = (cpu->r[R_AX] & ~0xffff) | ds;
		x86_loadseg(cpu, S_DS, ds);
	}
	if (x86_call(cpu, FPSEL(p), FPOFF(p)) != 0)
		w16_fatal("call to %04x:%04x failed", FPSEL(p), FPOFF(p));
	r = (cpu->r[R_AX] & 0xffff) | (cpu->r[R_DX] & 0xffff) << 16;
	restregs(&cbsave[--ncb], 0);
	return r;
}
