/*
 * trace.c -- with C:\MINT\XAMINT.TRC present, every module interface
 * and GEMDOS call is logged on its way in, and the process table every
 * ten idle seconds.
 */
#include <stddef.h>
#include "xamint.h"

#define	TT	10			/* bytes per tracing thunk */

int xa_tracing;
extern int debug_level;
static long real[1024];

#define	K(f)	{ offsetof(struct kentry, f) / 4, #f }
static const struct { short n; const char *name; } knames[] = {
	K(vec_proc.nap), K(vec_proc.sleep), K(vec_proc.wake), K(vec_proc.wakeselect),
	K(vec_proc.ikill), K(vec_proc.iwake), K(vec_proc.killgroup), K(vec_proc.raise),
	K(vec_proc.addtimeout), K(vec_proc.canceltimeout), K(vec_proc.addroottimeout),
	K(vec_proc.cancelroottimeout), K(vec_proc.create_process), K(vec_proc.kthread_create),
	K(vec_proc.kthread_exit), K(vec_proc.get_curproc), K(vec_proc.pid2proc),
	K(vec_proc.lookup_extension), K(vec_proc.attach_extension), K(vec_proc.detach_extension),
	K(vec_mem.kmalloc), K(vec_mem.kfree), K(vec_mem.umalloc), K(vec_mem.ufree),
	K(vec_mem.addr2mem), K(vec_mem.attach_region), K(vec_mem.detach_region),
	K(vec_fs.kernel_opendir), K(vec_fs.kernel_readdir), K(vec_fs.kernel_closedir),
	K(vec_fs.kernel_open), K(vec_fs.kernel_read), K(vec_fs.kernel_write),
	K(vec_fs.kernel_lseek), K(vec_fs.kernel_close), K(vec_fs.path2cookie),
	K(vec_fs.release_cookie), K(vec_module.load_modules), K(vec_module.register_trap2),
	K(vec_cnf.parse_cnf), K(vec_misc.get_toscookie), K(vec_misc.trap_1_emu),
	K(vec_misc.trap_13_emu), K(vec_misc.trap_14_emu), K(vec_xfs.getxattr),
	K(vec_xfs.readlink), { 0, 0 }
};

static void
wrap(long *p, int n)
{
	if (!*p)
		return;
	real[n] = *p;
	*p = (long)(xa_tthunks + TT * n);
}

void
trace_init(void)
{
	char hp[PLEN], st[HST_SIZE];
	long *k = (long *)&kentry, *d = (long *)&dos_tab;
	int i;

	if (host_path("C:\\MINT\\XAMINT.TRC", hp, 1) != 0 || hstat(hp, st, 0) < 0)
		return;
	xa_tracing = 1;
	debug_level = 2;
	for (i = 0; knames[i].name; i++)
		if (knames[i].n != offsetof(struct kentry, vec_proc.get_curproc) / 4)
			wrap(k + knames[i].n, knames[i].n);
	for (i = 0; i < 0x190; i++) {
		if (dos_trap[i] == (long (*)())d[i])
			wrap((long *)&dos_trap[i], 512 + i);
		real[512 + i] = d[i];
		if ((char *)d[i] < xa_thunks || (char *)d[i] >= xa_thunks + 8 * 1024)
			d[i] = (long)(xa_tthunks + TT * (512 + i));
	}
	xlog("tracing");
}

long
xa_trace(long n)
{
	int i;

	if (n >= 512)
		xlog("%d dos 0x%lx", curproc->pid, n - 512);
	else {
		for (i = 0; knames[i].name && knames[i].n != n; i++)
			;
		xlog("%d k %s", curproc->pid, knames[i].name ? knames[i].name : "?");
	}
	return real[n];
}

void
trace_procs(void)
{
	struct kp *k;

	long *q = *(long **)0x456;
	short i, n = *(short *)0x454;

	for (k = allprocs; k; k = k->all)
		xlog("  pid %d %s q %d cond %lx wake %ld%s", k->p.pid, k->p.name, k->p.wait_q,
			k->p.wait_cond, k->wake_at, k->zombie ? " zombie" : "");
	for (i = 0; i < n; i++)
		xlog("  vbl %d: %lx", i, q[i]);
}

/* a fault: logged; the process it hit ends */
void
xa_fault(long *regs)
{
	static short busy;
	short vec = *(short *)(regs + 15);
	unsigned short *f = (unsigned short *)((char *)(regs + 15) + 2);
	long pc = (long)f[1] << 16 | f[2];

	xlog("fault: vector %d pid %d (%s) sr %x pc %lx fmt %x [%x %x %x %x %x %x %x %x %x %x]",
		vec, curproc->pid, curproc->name, f[0], pc, f[3], f[4], f[5], f[6], f[7], f[8],
		f[9], f[10], f[11], f[12], f[13]);
	xlog("  d0-7 %lx %lx %lx %lx %lx %lx %lx %lx", regs[0], regs[1], regs[2], regs[3], regs[4],
		regs[5], regs[6], regs[7]);
	xlog("  a0-6 %lx %lx %lx %lx %lx %lx %lx usp %lx", regs[8], regs[9], regs[10], regs[11],
		regs[12], regs[13], regs[14], xa_usp());
	{
		static const short fsz[16] = { 8, 8, 12, 12, 16, 0, 0, 60, 58, 20, 32, 92 };
		long *sp = (long *)((char *)f + fsz[f[3] >> 12]);
		int i;

		for (i = 0; i < 32; i += 8)
			xlog("  sp+%d: %lx %lx %lx %lx %lx %lx %lx %lx", i * 4, sp[i], sp[i + 1],
				sp[i + 2], sp[i + 3], sp[i + 4], sp[i + 5], sp[i + 6], sp[i + 7]);
	}
	if (busy++ || curproc == rootproc) {
		xlog("fault in the fault path or the root: stopped");
		for (;;)
			xa_idle();
	}
	busy = 0;
	xa_splx(0x2000);
	proc_exit(KP(curproc), (vec == 4 ? SIGILL : vec == 5 ? SIGFPE : SIGSEGV) << 8);
}

void
fault_init(void)
{
	extern char xa_faults[];
	static const short v[] = { 2, 3, 4, 5, 8 };
	int i;

	for (i = 0; i < 5; i++)
		*(long *)(v[i] * 4L) = (long)(xa_faults + 6 * i);
}
