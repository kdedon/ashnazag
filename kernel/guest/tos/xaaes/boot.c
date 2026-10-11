/*
 * boot.c -- XAMINT.PRG: from the AUTO folder it stays resident and
 * takes exec_os, so the shell TOS starts after AUTO is this layer's
 * root.  The root hooks trap #1, trap #2 and the 50 Hz timer, adds
 * the MiNT cookie and starts XaAES in a loader process.
 */
#include "xamint.h"

#define	KM	"C:\\MINT\\XAAES\\XAAES.KM"
#define	TOSKEEP	(64 * 1024L)		/* left to TOS's own allocator */

static long
gemdos(int fn, long a, long b)
{
	register long r __asm__("d0");

	__asm__ __volatile__(
		"move.l %3,-(sp)\n\tmove.l %2,-(sp)\n\tmove.w %1,-(sp)\n\ttrap #1\n\tlea 10(sp),sp"
		: "=r"(r) : "g"(fn), "g"(a), "g"(b) : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
	return r;
}

static long
supexec(long (*f)(void))
{
	register long r __asm__("d0");

	__asm__ __volatile__(
		"move.l %1,-(sp)\n\tmove.w #38,-(sp)\n\ttrap #14\n\taddq.l #6,sp"
		: "=r"(r) : "g"(f) : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
	return r;
}

static long
take_exec_os(void)
{
	*(long *)0x4fe = (long)xa_osentry;
	return 0;
}

/* AUTO: resident when XaAES is installed, else gone */
long
xa_auto(BASEPAGE *bp)
{
	long fd = gemdos(0x3d, (long)KM, 0L);

	if (fd < 0) {
		gemdos(9, (long)"XaMiNT: no " KM ", not installed\r\n", 0);
		return 0;
	}
	gemdos(0x3e, fd << 16, 0);
	supexec(take_exec_os);
	gemdos(9, (long)"XaMiNT: XaAES starts after AUTO\r\n", 0);
	return 256 + bp->p_tlen + bp->p_dlen + bp->p_blen;
}

static void
loader(void *arg)
{
	long r;

	(void)arg;
	dir_set(KP(curproc), 2, "MINT\\XAAES");
	r = run_xaaes(KM);
	xlog("XaAES ended (%ld): the environment ends", r);
	hc(H_EXIT, 1, 0L);
}

/* the loader's basepage: the shell's, with XaAES's log in its environment */
static BASEPAGE *
loaderbase(BASEPAGE *bp)
{
	static const char log[] = "XAAES_LOGFILE=C:\\MINT\\XAAES.LOG";
	BASEPAGE *b = kmalloc_o(256, 0);
	const char *e = bp->p_env ? bp->p_env : "";
	long n;
	char *d;

	memcpy(b, bp, 256);
	for (n = 0; e[n] || e[n + 1]; n++)
		;
	d = kmalloc_o(n + sizeof log + 2, 0);
	memcpy(d, e, n + 1);
	if (n)
		n++;
	strcpy(d + n, log);
	d[n + sizeof log] = 0;
	b->p_env = d;
	return b;
}

/* exec_os: the shell process becomes MiNT's root */
void
xa_boot(BASEPAGE *bp)
{
	struct kp *k;
	long top, akp;

	gemdos(0x4a, (long)bp, bp->p_hitpa - bp->p_lowtpa - TOSKEEP);
	xa_super();
	file_init();
	xlog_open();
	xlog("XaMiNT: root basepage %lx, xa_auto at %lx", (long)bp, (long)xa_auto);
	top = bp->p_hitpa - TOSKEEP;
	mem_init(bp->p_lowtpa + 256, top);
	proc_init();
	KP(rootproc)->base = bp;
	rootproc->p_mem->base = bp;
	KP(rootproc)->dta = (char *)bp->p_cmdlin;
	dir_set(KP(rootproc), 2, "");
	extern void fd_std(struct kp *k);
	fd_std(KP(rootproc));
	dos_init();
	kentry_init();
	trace_init();
	if (cookie_get(0x5f414b50L, &akp) == 0) {
		global.gl_kbd = akp & 0xff;
		global.gl_lang = (akp >> 8) & 0xff;
	}
	if (cookie_get(0x5f435055L, &akp) == 0) {
		extern long mcpu;

		mcpu = akp;
	}
	global.tosvers = *(unsigned short *)(*(long *)0x4f2 + 2);
	cookie_set(0x4d694e54L, 0x0113);
	fault_init();
	old_trap1 = *(long *)0x84;
	*(long *)0x84 = (long)xa_trap1;
	old_trap2 = *(long *)0x88;
	*(long *)0x88 = (long)xa_trap2;
	old_etv = *(long *)0x400;
	*(long *)0x400 = (long)xa_tick;
	k = proc_new(KP(rootproc), "XaAES", 1, 0);
	k->kfunc = loader;
	k->base = loaderbase(bp);
	k->p.p_mem->base = k->base;
	k->p.domain = DOM_MINT;
	ready(k);
	idle_loop();
}
