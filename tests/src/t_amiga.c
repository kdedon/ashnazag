/*
 * t_amiga.c -- the Amiga container: amigaguest on a 68040.
 *
 * Registers amigaguest for character major 57 (modules in
 * /tests/aux/mod.d), loads and unloads it, and checks /dev/amiga's
 * ioctl validation.  Small guests (/tests/amiga/guest.bin, guest.s)
 * then run without Kickstart: enter and leave, SR and stack changes
 * through RTE, privilege and trap reflection, the interrupt mask with
 * VERTB, STOP, exit, exec, fork, two guests at once and unload while a
 * guest holds the module.  The session's host calls: a user's halt is
 * refused, root's reaches uadmin (recorded, not run) and the exit ends
 * the session.  With the local A4000 ROM, Kickstart runs
 * unpatched in a guest with AMIGAF_CENSUS for 40 s; the kernel logs
 * each custom-chip, CIA and other I/O register it touched at exit
 * ("amiga census" lines) and the guest's last fault.
 * With SYS: on a ufs volume (SCSI disk 0), startmig boots it from
 * /amiga/sys; host-side access times show DOS reading Startup-Sequence
 * and Workbench loading.  The boot extension binds container.card
 * before any screen opens; the RTG session then shows Workbench, a
 * drag on its backdrop changes the screen, IBrowse (when installed)
 * shows a local page and SIGTERM ends the session.  On a root with
 * TCP/IP, SYS:bsdtest uses bsdsocket.library against bsdsrv's servers.
 * Skips without guest support, the module or the 68040 path.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mkdev.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <termio.h>
#include <signal.h>
#include <poll.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>
#include <dirent.h>
#include <sys/resource.h>
#include "sys/mod.h"
#include "amigaio.h"
#include "rtgshare.h"
#include "hostfswire.h"
#include "t.h"

extern int getksym();

#define	MD	"/tests/aux/mod.d"
#define	GUEST	"/tests/amiga/guest.bin"
#define	ROM	"/etc/amiga/kicka4000.rom"
#define	SYSDEV	"/dev/dsk/c0d0s0"
#define	SYS	"/amiga/sys"
#define	BSDSRV	"/tests/amiga/bsdsrv"
#define	LOAD	0x1000L
#define	INTENA	0xdff09aL
#define	INTENAR	0xdff01cL

typedef long (*gfn)();
#define	G(i)	((gfn)((long *)LOAD)[i])
#define	getsr	G(0)
#define	setsr	G(1)
#define	wr16	G(2)
#define	rd16	G(3)
#define	urte	G(4)
#define	stopit	G(5)
#define	vblon	G(6)
#define	vblcount (*(volatile long *)((long *)LOAD)[7])
#define	dis	G(8)
#define	trp	G(9)
#define	hexit	G(10)
#define	hhalt	G(11)

static int afd = -1;

static void
nap(ms)
	int ms;
{
	long t0 = t_now_ms();

	while (t_now_ms() - t0 < ms)
		poll((struct pollfd *)0, 0L, (int)(ms - (t_now_ms() - t0)));
}

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

static int
E(r)
	int r;
{
	return r < 0 ? errno : 0;
}

static void
nothing(s)
	int s;
{
}

/* chip RAM at 0 with the guest image, /dev/amiga, entered; 0 or a code */
static int
enterf(flags)
	int flags;
{
	struct amigaenter ae;
	struct sigaction sa;
	int fd, n;

	if ((fd = open("/dev/zero", O_RDWR)) < 0 ||
	    mmap((caddr_t)0, AMIGA_CHIP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, fd, 0) == (caddr_t)-1)
		return 90;
	close(fd);
	if ((fd = open(GUEST, O_RDONLY)) < 0 || (n = read(fd, (char *)LOAD, 0x8000)) < 64)
		return 91;
	close(fd);
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGUSR2, &sa, (struct sigaction *)0);
	if ((afd = open("/dev/amiga", O_RDWR)) < 0)
		return 92;
	ae.ae_version = AMIGA_ABI_VERSION;
	ae.ae_chipsize = AMIGA_CHIP_SIZE;
	ae.ae_fastsize = 0;
	ae.ae_flags = flags;
	return ioctl(afd, AMIGAIOC_ENTER, &ae) < 0 ? 93 : 0;
}

static int
enter()
{
	return enterf(AMIGAF_PAL);
}

/* VERTB at level 3, counted by the guest */
static void
vbl()
{
	vblon();
	wr16(INTENA, 0xc020L);
}

static int
g_enter()
{
	struct amigastat st;
	struct amigaenter ae;
	int e;

	if ((e = enter()) != 0)
		return e;
	if (ioctl(afd, AMIGAIOC_STAT, &st) < 0 || st.as_pid != getpid())
		return 1;
	if ((getsr() & 0xff00) != 0x2700)
		return 2;
	ae.ae_version = AMIGA_ABI_VERSION;
	ae.ae_chipsize = AMIGA_CHIP_SIZE;
	ae.ae_fastsize = 0;
	ae.ae_flags = 0;
	if (ioctl(afd, AMIGAIOC_ENTER, &ae) == 0)
		return 3;
	if (ioctl(afd, AMIGAIOC_LEAVE, 0) < 0)
		return 4;
	if (E(ioctl(afd, AMIGAIOC_STAT, &st)) != ENXIO)
		return 5;
	if (ioctl(afd, AMIGAIOC_ENTER, &ae) < 0)
		return 6;
	if ((getsr() & 0xff00) != 0x2700 || E(ioctl(afd, 0x4199, 0)) != EINVAL)
		return 7;
	return 0;
}

static int
g_rte()
{
	int e;

	if ((e = enter()) != 0)
		return e;
	if ((e = urte(0x40000L)) != 0)
		return 10 + e;
	return (getsr() & 0xff00) == 0x2700 ? 0 : 20;
}

static int
g_mask()
{
	struct amigastat st;
	long c;
	int e;

	if ((e = enter()) != 0)
		return e;
	vbl();
	if (rd16(INTENAR) != 0x4020)
		return 1;
	nap(300);
	if (vblcount != 0)
		return 2;
	setsr(0x2000L);
	nap(300);
	if (vblcount == 0)
		return 3;
	setsr(0x2300L);
	c = vblcount;
	nap(300);
	if (vblcount != c)
		return 4;
	setsr(0x2200L);
	nap(300);
	if (vblcount == c)
		return 5;
	setsr(0x2700L);
	c = vblcount;
	nap(200);
	if (vblcount != c)
		return 6;
	if (ioctl(afd, AMIGAIOC_STAT, &st) < 0 || st.as_intr < vblcount)
		return 7;
	/* exit with the timer and VERTB live */
	setsr(0x2000L);
	return 0;
}

static int
g_stop()
{
	struct amigastat st;
	long c, t0, i;
	int e;

	if ((e = enter()) != 0)
		return e;
	vbl();
	for (i = 0; i < 5; i++) {
		c = vblcount;
		t0 = t_now_ms();
		stopit();
		if (vblcount == c)
			return 1 + i;
		if (t_now_ms() - t0 > 500)
			return 10 + i;
	}
	if (ioctl(afd, AMIGAIOC_STAT, &st) < 0 || st.as_stop != 5)
		return 20;
	return 0;
}

static int
g_exit()
{
	int e;

	if ((e = enter()) != 0)
		return e;
	exit(7);
}

static int
g_exec()
{
	int e;

	if ((e = enter()) != 0)
		return e;
	execl("/tests/t_amiga", "t_amiga", "stat", (char *)0);
	return 1;
}

static int
g_fork()
{
	struct amigastat st;
	pid_t p;
	int e;

	if ((e = enter()) != 0)
		return e;
	if ((p = fork()) == 0)
		_exit(0);
	if (p >= 0)
		return 1;
	if (ioctl(afd, AMIGAIOC_STAT, &st) < 0 || (getsr() & 0xff00) != 0x2700)
		return 3;
	return 0;
}

static int (*gtab[])() = { g_enter, g_rte, g_mask, g_stop, g_exit, g_exec, g_fork };
static char *gname[] = { "enter_leave", "rte_user", "intmask", "stop", "exit", "exec", "fork" };
static int gwant[] = { 0, 0, 0, 0, 7, 0, 0 };

static void
guests()
{
	int i, st;
	pid_t p;

	for (i = 0; i < sizeof gtab / sizeof gtab[0]; i++) {
		if ((p = fork()) == 0)
			_exit(gtab[i]());
		if (t_waitchild(p, &st, 20) < 0)
			t_fail(gname[i], "timed out");
		else
			t_check(gname[i], WIFEXITED(st) && WEXITSTATUS(st) == gwant[i],
			    "status 0x%x", st);
	}
}

/* a kernel or module long through /dev/kmem */
static int
kmemrw(addr, val, wr)
	unsigned long addr;
	long *val;
	int wr;
{
	int fd = open("/dev/kmem", wr ? O_RDWR : O_RDONLY), ok;

	if (fd < 0)
		return -1;
	ok = lseek(fd, (off_t)addr, 0) != -1 &&
	    (wr ? write(fd, (char *)val, 4) : read(fd, (char *)val, 4)) == 4;
	close(fd);
	return ok ? 0 : -1;
}

static int
setsym(name, v)
	char *name;
	long v;
{
	unsigned long a = 0, info;

	if (getksym(name, &a, &info) < 0)
		return -1;
	return kmemrw(a, &v, 1);
}

static long
getsym(name)
	char *name;
{
	unsigned long a = 0, info;
	long v;

	if (getksym(name, &a, &info) < 0 || kmemrw(a, &v, 0) < 0)
		return -1;
	return v;
}

/* a guest as uid (in the display group) asks for a halt: 0 refused, else a code */
static int
g_halt(uid)
	int uid;
{
	int e;

	if (uid && (setgid(25) < 0 || setuid(uid) < 0))
		return 80;
	if ((e = enter()) != 0)
		return e;
	if ((e = hhalt((long)afd)) != EPERM)
		return 10 + e;
	return 0;
}

/*
 * The session's host calls, as its Tools menu makes them: Log Out's exit
 * ends the guest, a user's Shut Down is refused, and root's is recorded
 * in place of a halt, the guest killed instead.
 */
static void
exits()
{
	int st, e;
	pid_t p;

	if (setsym("guest_adcall", 0L) < 0 || setsym("guest_adtest", 1L) < 0 ||
	    getsym("guest_adtest") != 1) {
		t_skip("exit", "no guest_adtest in guestcore");
		return;
	}
	if ((p = fork()) == 0)
		_exit((e = enter()) != 0 ? e : (hexit(0L), 99));
	if (t_waitchild(p, &st, 20) < 0)
		t_fail("exit_logout", "timed out");
	else
		t_check("exit_logout", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status 0x%x", st);
	if ((p = fork()) == 0)
		_exit(g_halt(100));
	if (t_waitchild(p, &st, 20) < 0)
		t_fail("exit_user_refused", "timed out");
	else
		t_check("exit_user_refused", WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
		    getsym("guest_adcall") == 0, "status 0x%x, uadmin %#lx", st,
		    getsym("guest_adcall"));
	if ((p = fork()) == 0)
		_exit(g_halt(0) == 10 ? 98 : 97);
	if (t_waitchild(p, &st, 20) < 0)
		t_fail("exit_root_halt", "timed out");
	else
		t_check("exit_root_halt", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL &&
		    getsym("guest_adcall") == 0x200, "status 0x%x, uadmin %#lx", st,
		    getsym("guest_adcall"));
	setsym("guest_adcall", 0L);
	setsym("guest_adtest", 0L);
}

/* microseconds per INTENA write and per reflected trap */
static void
cost()
{
	long t0, t1, t2;
	int st;
	pid_t p;

	if ((p = fork()) == 0) {
		if (enter() != 0)
			_exit(1);
		t0 = t_now_ms();
		dis(2000L);
		t1 = t_now_ms();
		trp(4000L);
		t2 = t_now_ms();
		printf("INFO amiga.cost: INTENA write %ld us, trap round trip %ld us\n",
		    (t1 - t0) / 4, (t2 - t1) / 4);
		fflush(stdout);
		_exit(0);
	}
	t_waitchild(p, &st, 60);
}

/* one byte, or 0 after 10 s: a guest that failed to enter never writes */
static int
rd1(fd)
	int fd;
{
	struct pollfd p;
	char c;

	p.fd = fd;
	p.events = POLLIN;
	return poll(&p, 1L, 10000) == 1 && read(fd, &c, 1) == 1;
}

/* two guests at once: VERTB enabled in one only */
static void
two()
{
	int go[2], rdy[2], i, st[2], ok;
	pid_t p[2];
	char c;

	pipe(go);
	pipe(rdy);
	for (i = 0; i < 2; i++)
		if ((p[i] = fork()) == 0) {
			int e = enter();

			if (e)
				_exit(e);
			if (i == 0)
				vbl();
			setsr(0x2000L);
			write(rdy[1], "e", 1);
			/* the guest's VBL interrupts the wait */
			while ((e = read(go[0], &c, 1)) < 0 && errno == EINTR)
				;
			if (e != 1)
				_exit(80);
			nap(500);
			if (i == 0)
				_exit(vblcount > 5 ? 0 : 1);
			_exit(vblcount == 0 && rd16(INTENAR) == 0 ? 0 : 2);
		}
	ok = rd1(rdy[0]) && rd1(rdy[0]);
	t_check("two_entered", ok, "a guest did not enter");
	write(go[1], "gg", 2);
	for (i = 0; i < 2; i++)
		if (t_waitchild(p[i], &st[i], 20) < 0)
			st[i] = -1;
	t_check("two_instances", st[0] == 0 && st[1] == 0, "status 0x%x 0x%x", st[0], st[1]);
	close(go[0]); close(go[1]); close(rdy[0]); close(rdy[1]);
}

/* the guest's module reference keeps amigaguest loaded */
static void
unloadbusy()
{
	int rdy[2], st, id;
	pid_t p;
	char c;

	pipe(rdy);
	if ((p = fork()) == 0) {
		if (enter())
			_exit(1);
		close(afd);
		write(rdy[1], "e", 1);
		for (;;)
			pause();
	}
	close(rdy[1]);
	close(afd);
	afd = -1;
	if (!t_check("unload_busy_setup", read(rdy[0], &c, 1) == 1, "guest did not enter")) {
		kill(p, SIGKILL);
		t_waitchild(p, &st, 10);
		return;
	}
	id = modid("amigaguest");
	t_check("unload_busy", id > 0 && E(moduload(id)) == EBUSY, "id %d: %s", id, T_ERR);
	kill(p, SIGKILL);
	t_waitchild(p, &st, 10);
	t_check("unload_after_exit", moduload(id) == 0 && modid("amigaguest") < 0, "%s", T_ERR);
	close(rdy[0]);
}

static void
ioctls()
{
	struct amigainfo ai;
	struct amigaenter ae;
	struct amigastat st;
	int id;

	t_check("info", ioctl(afd, AMIGAIOC_INFO, &ai) == 0 && ai.ai_version == AMIGA_ABI_VERSION &&
	    (ai.ai_features & AMIGA_FEAT_BASE), "%s", T_ERR);
	t_info("features", "%lx", ai.ai_features);
	t_check("stat_unentered", E(ioctl(afd, AMIGAIOC_STAT, &st)) == ENXIO, "%s", T_ERR);
	t_check("leave_unentered", E(ioctl(afd, AMIGAIOC_LEAVE, 0)) == ENXIO, "%s", T_ERR);
	t_check("bad_cmd", E(ioctl(afd, 0x4199, 0)) == ENXIO, "%s", T_ERR);
	ae.ae_version = 2;
	ae.ae_chipsize = AMIGA_CHIP_SIZE;
	ae.ae_fastsize = 0;
	ae.ae_flags = 0;
	t_check("enter_version", E(ioctl(afd, AMIGAIOC_ENTER, &ae)) == EINVAL, "%s", T_ERR);
	ae.ae_version = AMIGA_ABI_VERSION;
	ae.ae_chipsize = 0x100000;
	t_check("enter_chipsize", E(ioctl(afd, AMIGAIOC_ENTER, &ae)) == EINVAL, "%s", T_ERR);
	ae.ae_chipsize = AMIGA_CHIP_SIZE;
	ae.ae_fastsize = 0x80000;
	t_check("enter_fastalign", E(ioctl(afd, AMIGAIOC_ENTER, &ae)) == EINVAL, "%s", T_ERR);
	ae.ae_fastsize = AMIGA_FAST_MAX + 0x100000;
	t_check("enter_fastmax", E(ioctl(afd, AMIGAIOC_ENTER, &ae)) == EINVAL, "%s", T_ERR);
	ae.ae_fastsize = 0;
	ae.ae_flags = 8;
	t_check("enter_flags", E(ioctl(afd, AMIGAIOC_ENTER, &ae)) == EINVAL, "%s", T_ERR);
	t_check("enter_efault", E(ioctl(afd, AMIGAIOC_ENTER, (char *)1)) == EFAULT, "%s", T_ERR);
	unlink("/tmp/amiga1");
	t_check("open_minor", mknod("/tmp/amiga1", S_IFCHR | 0600, makedev(AMIGA_MAJOR, 1)) == 0 &&
	    open("/tmp/amiga1", O_RDWR) < 0 && errno == ENXIO, "%s", T_ERR);
	unlink("/tmp/amiga1");
	t_check("loaded", modid("amigaguest") > 0 && modid("guestcore") > 0, "not in modstat");
	close(afd);
	afd = -1;
	id = modid("amigaguest");
	t_check("unload", id > 0 && moduload(id) == 0 && modid("amigaguest") < 0, "%s", T_ERR);
	t_check("reload", (afd = open("/dev/amiga", O_RDWR)) >= 0 && modid("amigaguest") > 0,
	    "%s", T_ERR);
}

/*
 * Kickstart in a guest of its own, chip RAM only and no boot extension,
 * for the register census; it runs until it waits for a boot device.
 */
static int
g_rom()
{
	int fd, e;

	if (mmap((caddr_t)0xf80000, 0x80000, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, open("/dev/zero", O_RDWR), 0) == (caddr_t)-1)
		return 94;
	if ((fd = open(ROM, O_RDONLY)) < 0 || read(fd, (char *)0xf80000, 0x80000) != 0x80000)
		return 95;
	close(fd);
	if ((e = enterf(AMIGAF_PAL | AMIGAF_CENSUS)) != 0)
		return e;
	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "d" (0x400L),
	    "a" (*(long *)0xf80004));
	return 96;
}

static void
rom()
{
	int st, i;
	pid_t p;

	if ((p = fork()) == 0)
		_exit(g_rom());
	for (i = 0; i < 40 && waitpid(p, &st, WNOHANG) != p; i++)
		sleep(1);
	if (i == 40) {
		kill(p, SIGKILL);
		t_waitchild(p, &st, 10);
		t_info("rom", "Kickstart ran 40 s (census above)");
	}
	t_check("rom_census", i == 40, "Kickstart ended after %d s: status 0x%x", i, st);
}

/* nonzero once the guest has read SYS:path since t0 */

/*
 * The card's drawing functions (draw.bin at DRAW) on bitmaps in this
 * process, against plain C: fills, inversions, copies in every direction,
 * templates in each draw mode, P96's own function for what they decline,
 * saved registers, and the host pointer put back before drawing.
 */
#define DRAW	0x24000000UL
#define DW	256
#define DH	64
static unsigned char dbm[2][DW * DH], dref[2][DW * DH], dtpl[16 * DH];
static long dreg[22];
static struct { long mem; short bpr, pad; long fmt; } dri[2];
static struct { long mem; short bpr; unsigned char xo, mode; long fg, bg; } dtp;
static long dbi[512];

static int
dcall(n)
	int n;
{
	static long in[11];
	int i, ok = 1;

	memcpy((char *)in, (char *)dreg, sizeof in);
	((int (*)())((long *)DRAW)[5])(((long *)DRAW)[n], dreg);
	for (i = 2; i < 8; i++)
		ok &= dreg[11 + i] == in[i];
	return ok & (dreg[11 + 10] == in[10]);
}

static int
drand(n)
	int n;
{
	return rand() % n;
}

static void
dreset()
{
	int i;

	for (i = 0; i < DW * DH; i++)
		dbm[0][i] = dref[0][i] = rand(), dbm[1][i] = dref[1][i] = rand();
}

/* rectangle x,y w,h fitting DW x DH */
static void
drect(v)
	int v[4];
{
	v[2] = 1 + drand(DW - 1);
	v[3] = 1 + drand(DH - 1);
	v[0] = drand(DW - v[2] + 1);
	v[1] = drand(DH - v[3] + 1);
}

static void
drawtest()
{
	volatile struct mig_rtg *s;
	unsigned char *p, tmp[DW * DH];
	int fd, i, k, x, y, n, fail[8], v[4], w[4], bit, c, mode;
	long *deflt;

	struct rlimit rl;

	if ((fd = open("/tests/amiga/draw.bin", O_RDONLY)) < 0) {
		t_skip("draw", "no draw.bin");
		return;
	}
	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	if (mmap((caddr_t)DRAW, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED,
	    open("/dev/zero", O_RDWR), 0) == (caddr_t)-1) {
		t_check("draw_map", 0, "code: %s", T_ERR);
		return;
	}
	if (mmap((caddr_t)MIG_RTG_BASE, MIG_RTG_HEADER_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_FIXED, open("/dev/zero", O_RDWR), 0) == (caddr_t)-1) {
		t_check("draw_map", 0, "header: %s", T_ERR);
		return;
	}
	if (mmap((caddr_t)0xf7f000, 4096, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_FIXED, open("/dev/zero", O_RDWR), 0) == (caddr_t)-1) {
		t_check("draw_map", 0, "doorbell: %s", T_ERR);
		return;
	}
	read(fd, (char *)DRAW, 65536);
	close(fd);
	deflt = (long *)((long *)DRAW)[7];
	s = (volatile struct mig_rtg *)MIG_RTG_BASE;
	for (i = 0; i < 2; i++) {
		dri[i].mem = (long)dbm[i];
		dri[i].bpr = DW;
		dri[i].fmt = 1;
	}
	/* P96's own functions: the default slots */
	for (i = 386; i <= 434; i += 8)
		*(long *)((char *)dbi + i) = ((long *)DRAW)[6];
	srand(1);
	memset(fail, 0, sizeof fail);
	for (k = 0; k < 300; k++) {
		dreset();
		memset(dreg, 0, sizeof dreg);
		dreg[8] = (long)dbi;
		dreg[9] = (long)&dri[0];
		dreg[7] = 1;
		drect(v);
		dreg[0] = v[0]; dreg[1] = v[1]; dreg[2] = v[2]; dreg[3] = v[3];
		/* a pointer drawn over part of the screen bitmap, sometimes */
		s->pshown = k & 1;
		s->porigin = (long)dbm[0];
		s->pstride = DW;
		s->px0 = drand(DW - 16); s->py0 = drand(DH - 20);
		s->px1 = s->px0 + 1 + drand(16); s->py1 = s->py0 + 1 + drand(20);
		*(long *)0xf7fffc = 0;
		if (s->pshown)
			for (y = s->py0; y < s->py1; y++)
				for (x = s->px0; x < s->px1; x++) {
					c = y * DW + x;
					s->psave[y - s->py0][x - s->px0] = rand();
					/* some pixels drawn over since: they stay */
					s->pdrawn[y - s->py0][x - s->px0] = rand() & 1 ? dbm[0][c] : dbm[0][c] + 1;
				}
		switch (k % 5) {
		case 0:		/* fill */
			dreg[4] = rand();
			dreg[5] = 255;
			n = dcall(0);
			break;
		case 1:		/* invert */
			dreg[4] = rand();
			n = dcall(1);
			break;
		case 2:		/* copy within one bitmap */
			drect(w);
			v[2] = w[2]; v[3] = w[3];
			drect(w);
			if (w[0] + v[2] > DW) w[0] = DW - v[2];
			if (w[1] + v[3] > DH) w[1] = DH - v[3];
			if (v[0] + v[2] > DW) v[0] = DW - v[2];
			if (v[1] + v[3] > DH) v[1] = DH - v[3];
			if (k % 3 == 0) w[1] = v[1];
			dreg[0] = v[0]; dreg[1] = v[1]; dreg[2] = w[0]; dreg[3] = w[1];
			dreg[4] = v[2]; dreg[5] = v[3]; dreg[6] = 255;
			n = dcall(2);
			break;
		case 3:		/* copy between bitmaps */
			drect(w);
			v[2] = w[2]; v[3] = w[3];
			drect(w);
			if (w[0] + v[2] > DW) w[0] = DW - v[2];
			if (w[1] + v[3] > DH) w[1] = DH - v[3];
			if (v[0] + v[2] > DW) v[0] = DW - v[2];
			if (v[1] + v[3] > DH) v[1] = DH - v[3];
			dreg[9] = (long)&dri[1];
			dreg[10] = (long)&dri[0];
			dreg[0] = v[0]; dreg[1] = v[1]; dreg[2] = w[0]; dreg[3] = w[1];
			dreg[4] = v[2]; dreg[5] = v[3]; dreg[6] = 0xc0;
			n = dcall(3);
			break;
		default:	/* template */
			for (i = 0; i < sizeof dtpl; i++)
				dtpl[i] = rand();
			dtp.mem = (long)dtpl;
			dtp.bpr = 8 + 2 * drand(4);
			if (v[2] > dtp.bpr * 8 - 15)
				v[2] = dtp.bpr * 8 - 15;
			dreg[2] = v[2];
			dtp.xo = drand(16);
			dtp.mode = drand(3) | (rand() & 4);
			dtp.fg = rand();
			dtp.bg = rand();
			dreg[4] = (dtp.mode & 3) == 2 ? rand() & 255 : 255;
			dreg[10] = (long)&dtp;
			n = dcall(4);
			break;
		}
		/* the reference: the pointer put back, then the drawing */
		p = dref[0];
		if (k & 1) {
			int hit;
			x = dreg[0]; y = dreg[1];
			/* the rectangles the function touches on the screen bitmap */
			hit = 0;
			for (i = 0; i < 2; i++) {
				int rx, ry, rw, rh;
				if (k % 5 < 2 || k % 5 == 4) {
					if (i) break;
					rx = v[0]; ry = v[1]; rw = v[2]; rh = v[3];
				} else if (k % 5 == 2) {
					rx = i ? w[0] : v[0]; ry = i ? w[1] : v[1]; rw = v[2]; rh = v[3];
				} else {
					if (!i) continue;
					rx = w[0]; ry = w[1]; rw = v[2]; rh = v[3];
				}
				if (rx < s->px1 && rx + rw > s->px0 && ry < s->py1 && ry + rh > s->py0)
					hit = 1;
			}
			if (hit) {
				for (y = s->py0; y < s->py1; y++)
					for (x = s->px0; x < s->px1; x++)
						if (p[y * DW + x] == s->pdrawn[y - s->py0][x - s->px0])
							p[y * DW + x] = s->psave[y - s->py0][x - s->px0];
				if (s->pshown || *(long *)0xf7fffc != 1 || s->pad)
					fail[5]++;
			} else if (!s->pshown || *(long *)0xf7fffc)
				fail[5]++;
		}
		switch (k % 5) {
		case 0:
			for (y = v[1]; y < v[1] + v[3]; y++)
				memset(p + y * DW + v[0], (int)dreg[4] & 255, v[2]);
			break;
		case 1:
			for (y = v[1]; y < v[1] + v[3]; y++)
				for (x = v[0]; x < v[0] + v[2]; x++)
					p[y * DW + x] ^= dreg[4];
			break;
		case 2:
		case 3:
			memcpy((char *)tmp, (char *)(k % 5 == 2 ? dref[0] : dref[1]), sizeof tmp);
			for (y = 0; y < v[3]; y++)
				memcpy((char *)p + (w[1] + y) * DW + w[0],
				    (char *)tmp + (v[1] + y) * DW + v[0], v[2]);
			break;
		default:
			mode = dtp.mode;
			for (y = 0; y < v[3]; y++)
				for (x = 0; x < v[2]; x++) {
					bit = dtp.xo + x;
					c = dtpl[y * dtp.bpr + bit / 8] >> (7 - bit % 8) & 1;
					if (mode & 4) c = !c;
					i = (v[1] + y) * DW + v[0] + x;
					if ((mode & 3) >= 2) { if (c) p[i] ^= dreg[4]; }
					else if (c) p[i] = dtp.fg;
					else if (mode & 1) p[i] = dtp.bg;
				}
			break;
		}
		if (memcmp((char *)dbm[0], (char *)dref[0], DW * DH) ||
		    memcmp((char *)dbm[1], (char *)dref[1], DW * DH))
			fail[k % 5]++;
		if (!n)
			fail[6]++;
	}
	t_check("draw_fill", !fail[0], "%d of 60 differ", fail[0]);
	t_check("draw_invert", !fail[1], "%d of 60 differ", fail[1]);
	t_check("draw_copy", !fail[2], "%d of 60 differ", fail[2]);
	t_check("draw_copy2", !fail[3], "%d of 60 differ", fail[3]);
	t_check("draw_template", !fail[4], "%d of 60 differ", fail[4]);
	t_check("draw_pointer", !fail[5], "%d of 150 wrong", fail[5]);
	t_check("draw_registers", !fail[6], "%d calls changed saved registers", fail[6]);
	/* what the functions decline goes to P96's own */
	*deflt = 0;
	s->pshown = 0;
	dreg[8] = (long)dbi; dreg[9] = (long)&dri[0];
	dreg[0] = dreg[1] = 0; dreg[2] = dreg[3] = 4; dreg[5] = 0x0f; dreg[7] = 1;
	dcall(0);
	dreg[5] = 255; dreg[7] = 9;
	dcall(0);
	t_check("draw_default", *deflt == 2, "P96's own called %ld times, not 2", *deflt);
	munmap((caddr_t)DRAW, 65536);
	munmap((caddr_t)MIG_RTG_BASE, MIG_RTG_HEADER_SIZE);
	munmap((caddr_t)0xf7f000, 4096);
}

static int
used(path, t0)
	char *path;
	time_t t0;
{
	char b[128];
	struct stat sb;

	sprintf(b, "%s/%s", SYS, path);
	return stat(b, &sb) == 0 && sb.st_atime >= t0;
}

/* one request to the test host on /dev/term/b; its reply, or 0 */
static char *
host(req)
	char *req;
{
	static char line[256];
	static int fd = -1, seq;
	struct termio t;
	struct pollfd p;
	char buf[160], *r;
	int i, n;

	if (fd < 0) {
		if ((fd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
			return 0;
		if (ioctl(fd, TCGETA, &t) == 0) {
			t.c_iflag = IGNCR;
			t.c_oflag = 0;
			t.c_lflag = ICANON;
			t.c_cflag |= CREAD | CLOCAL;
			ioctl(fd, TCSETAF, &t);
		}
	}
	sprintf(buf, "@@ %d %s\n", ++seq, req);
	write(fd, buf, strlen(buf));
	sprintf(buf, "@@ok %d ", seq);
	p.fd = fd;
	p.events = POLLIN;
	for (i = 0; i < 40; i++) {
		if (poll(&p, 1, 500) <= 0 || (n = read(fd, line, sizeof line - 1)) <= 0)
			continue;
		line[n] = 0;
		line[strcspn(line, "\n")] = 0;
		if ((r = strstr(line, buf)) != 0)
			return r + strlen(buf);
	}
	return 0;
}

/* screen dumps NAME a second apart until two match (at most 20 s) */
static void
settle(name)
	char *name;
{
	char req[64], *r;
	int k;

	for (k = 0; k < 20; k++) {
		sprintf(req, "shot %s", name);
		host(req);
		sleep(1);
		host("shot amiga_tmp");
		sprintf(req, "cmp %s amiga_tmp", name);
		if ((r = host(req)) == 0 || strcmp(r, "same") == 0)
			return;
	}
}

/* box of the pixels that differ between dumps A and B, in rows Y0 and below */
static int
pbox(a, b, y0, v)
	char *a, *b;
	int y0, v[4];
{
	char req[80], *r;

	v[0] = v[1] = v[2] = v[3] = -1;
	sprintf(req, "box %s %s %d", a, b, y0);
	return (r = host(req)) != 0 && sscanf(r, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4;
}

/*
 * Dump NAME each second until its box against BASE is at least MINH rows
 * high and the same three times running (at most 20 s): motion has stopped.
 */
static void
track(base, name, y0, minh, v)
	char *base, *name;
	int y0, minh, v[4];
{
	char req[64];
	int k, n, p[4];

	sprintf(req, "shot %s", name);
	p[0] = -2;
	for (k = n = 0; k < 20 && n < 2; k++) {
		sleep(1);
		host(req);
		if (!pbox(base, name, y0, v) || v[3] - v[1] < minh)
			n = 0;
		else if (memcmp(v, p, sizeof p) == 0)
			n++;
		else
			n = 0;
		memcpy(p, v, sizeof p);
	}
}

/* a pointer or lasso's near edge at V */
static int
at(e, v)
	int e, v;
{
	return e >= v - 2 && e <= v + 2;
}

/* its far edge: V plus at most the pointer's size */
static int
near(e, v)
	int e, v;
{
	return e >= v && e < v + 24;
}

/*
 * IBrowse, when SYS: has it: the startup's waiting script starts it on
 * SYS:ibgo with a local page; its window must change at least 300x200
 * pixels and the page must be read.
 */
static void
ibrowse()
{
	char b[64], *r;
	time_t t0;
	int k, v[4];

	sprintf(b, "%s/IBrowse/IBrowse", SYS);
	if (access(b, 0) != 0) {
		t_skip("ibrowse", "no IBrowse in SYS:");
		return;
	}
	settle("amiga_ib0");
	t0 = time((time_t *)0);
	sprintf(b, "%s/ibgo", SYS);
	close(creat(b, 0644));
	for (k = 0; k < 90 && !used("ibtest.html", t0); k++)
		sleep(1);
	t_check("ibrowse_start", used("IBrowse/IBrowse", t0), "SYS:IBrowse/IBrowse never read");
	t_check("ibrowse_page", k < 90, "SYS:ibtest.html never read");
	track("amiga_ib0", "amiga_ibrowse", 0, 200, v);
	t_info("ibrowse_window", "changed %d,%d-%d,%d", v[0], v[1], v[2], v[3]);
	t_check("ibrowse_window", v[2] - v[0] >= 300 && v[3] - v[1] >= 200,
	    "changed %d,%d-%d,%d", v[0], v[1], v[2], v[3]);
	/* the demo's notice covers the page until confirmed */
	host("key ret");
	settle("amiga_ibpage");
	r = host("cmp amiga_ibrowse amiga_ibpage");
	t_info("ibrowse_notice", "after Return: %s", r ? r : "no reply");
}

/*
 * bsdsocket.library: SYS:bsdtest starts on SYS:bsdgo and talks to
 * bsdsrv, which prints a "name ok detail" line per check.
 */
static void
bsdsock()
{
	char b[160], name[32];
	FILE *f;
	int ok, k, n = 0;

	sprintf(b, "%s/bsdtest", SYS);
	if (access(b, 0) != 0 || access(BSDSRV, 1) != 0) {
		t_skip("bsdsock", "no bsdtest in SYS:");
		return;
	}
	if (access("/usr/lib/libsocket.so", 0) != 0 || access("/dev/tcp", 0) != 0) {
		t_skip("bsdsock", "no TCP/IP on this root");
		return;
	}
	if ((f = popen(BSDSRV " " SYS "/bsdgo", "r")) == 0) {
		t_check("bsdsock", 0, "popen: %s", T_ERR);
		return;
	}
	while (fgets(b, sizeof b, f) != 0) {
		b[strcspn(b, "\n")] = 0;
		if (sscanf(b, "%31s %d %n", name, &ok, &k) == 2) {
			t_check(name, ok, "%s", b + k);
			n++;
		} else
			t_info("bsdsock", "%s", b);
	}
	pclose(f);
	t_check("bsdsock", n > 0, "bsdsrv reported nothing");
}

/*
 * The sound driver: S:sndtest runs ahitest on SYS:sndgo, half a second
 * at 22050 Hz through container.audio without AHI.  The sound helper
 * logs what it took when play stops: frames, player passes (the
 * driver's timing hook) and the PORTS kicks that woke the mixing task.
 */
static void
sound()
{
	char b[64], line[256], *s;
	unsigned fr = 0, pa = 0, ki = 0;
	int k, hz = 0, ch = 0, got = 0;
	FILE *f;

	sprintf(b, "%s/sndgo", SYS);
	close(creat(b, 0644));
	for (k = 0; k < 30 && !got; k++) {
		sleep(1);
		if ((f = fopen("/tmp/startmig.log", "r")) == 0)
			continue;
		while (fgets(line, sizeof line, f) != 0)
			if ((s = strstr(line, "sound: stopped:")) != 0)
				got = sscanf(s, "sound: stopped: %u frames, %u passes, %u kicks",
				    &fr, &pa, &ki) == 3;
			else if ((s = strstr(line, "sound: ")) != 0)
				sscanf(s, "sound: %d Hz, %d channels", &hz, &ch);
		fclose(f);
	}
	unlink(b);
	t_check("sound_start", hz == 22050 && ch == 2, "%d Hz, %d channels", hz, ch);
	t_check("sound_stop", got, "no stop logged in %d s", k);
	t_info("sound", "%u frames, %u passes, %u kicks", fr, pa, ki);
	t_check("sound_frames", fr >= 6615 && fr <= 22050, "%u frames", fr);
	t_check("sound_passes", pa >= 15, "%u passes", pa);
	t_check("sound_kicks", ki >= 1, "%u kicks", ki);
}

static time_t migt0;
static void cpu();

/* guest P's traps so far: bus faults, privileged instructions, interrupts */
static void
traps(p, v)
	pid_t p;
	long v[4];
{
	struct amigawait aw;
	struct amigastat st;
	int fd;

	memset((char *)&aw, 0, sizeof aw);
	aw.aw_pid = p;
	aw.aw_stat = &st;
	v[0] = v[1] = v[2] = 0;
	v[3] = t_now_ms();
	if ((fd = open("/dev/amiga", O_RDWR)) < 0)
		return;
	if (ioctl(fd, AMIGAIOC_GSTAT, &aw) == 0) {
		v[0] = st.as_fault;
		v[1] = st.as_priv;
		v[2] = st.as_intr;
	} else
		t_info("traps", "%s", T_ERR);
	close(fd);
}

/* traps per second since V0 */
static void
trapinfo(name, p, v0)
	char *name;
	pid_t p;
	long v0[4];
{
	long v[4], ms;

	traps(p, v);
	ms = v[3] - v0[3] > 0 ? v[3] - v0[3] : 1;
	t_info(name, "in %ld ms: %ld faults/s, %ld privileged/s, %ld interrupts/s", ms,
	    (v[0] - v0[0]) * 1000 / ms, (v[1] - v0[1]) * 1000 / ms, (v[2] - v0[2]) * 1000 / ms);
}

/*
 * CPU samples while idle and while the pointer moves; a click on the
 * Amiga disk icon timed until its highlight shows.
 */
static void
pointer(p)
	pid_t p;
{
	char *r;
	long t0, g0[2], g1[2], h0[2], h1[2], tv[4];
	int k;

	settle("amiga_p0");
	t_info("phase", "idle profile at %ld s", (long)(time((time_t *)0) - migt0));
	traps(p, tv);
	host("prof 3 idle");
	nap(3500);
	trapinfo("idle_traps", p, tv);
	traps(p, tv);
	t_info("phase", "pointer profile at %ld s", (long)(time((time_t *)0) - migt0));
	cpu(p, 0, g0);
	cpu(p, 1, h0);
	t0 = t_now_ms();
	host("prof 3 point");
	host("glide 60 3 2 16");
	nap(3500);
	cpu(p, 0, g1);
	cpu(p, 1, h1);
	t_info("point_cpu", "in %ld ms: guest user %ld sys %ld, helpers user %ld sys %ld",
	    t_now_ms() - t0, g1[0] - g0[0], g1[1] - g0[1], h1[0] - h0[0], h1[1] - h0[1]);
	trapinfo("point_traps", p, tv);
	host("move -2000 -2000 50");
	host("move 50 77 50");
	settle("amiga_c0");
	t_info("phase", "click at %ld s", (long)(time((time_t *)0) - migt0));
	t0 = t_now_ms();
	host("button 1 50");
	host("button 0 0");
	for (k = 0; k < 30; k++) {
		host("shot amiga_c1");
		if ((r = host("cmp amiga_c0 amiga_c1")) != 0 && strncmp(r, "diff", 4) == 0)
			break;
	}
	t_info("click_time", "icon changed after %ld ms (%s)", t_now_ms() - t0, r ? r : "no reply");
	host("move 400 300 50");
	host("click 1");
	settle("amiga_c2");
}

/* the user and system CPU ms of P, or with KIDS of its children */
static void
cpu(p, kids, v)
	pid_t p;
	int kids;
	long v[2];
{
	char path[64];
	prstatus_t s;
	prpsinfo_t ps;
	DIR *d;
	struct dirent *e;
	int fd;

	v[0] = v[1] = 0;
	if ((d = opendir("/proc")) == 0)
		return;
	while ((e = readdir(d)) != 0) {
		if (e->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/%s", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && (kids ? ps.pr_ppid : ps.pr_pid) == p &&
		    ioctl(fd, PIOCSTATUS, &s) == 0) {
			v[0] += s.pr_utime.tv_sec * 1000L + s.pr_utime.tv_nsec / 1000000;
			v[1] += s.pr_stime.tv_sec * 1000L + s.pr_stime.tv_nsec / 1000000;
		}
		close(fd);
	}
	closedir(d);
}

/*
 * The test host's PC samples for SECS s: ms of all work (guest, helpers,
 * kernel; not the idle loop) and ms to the end of the last busy 100 ms.
 */
static void
busy(secs, name)
	int secs;
	char *name;
{
	static long idle;
	char req[64], *r;
	long b, e;

	if (!idle) {
		FILE *f = fopen("/tests/ksyms", "r");
		char sym[64];
		unsigned long a;

		idle = -1;
		while (f && fscanf(f, "%63s %lx", sym, &a) == 2)
			if (strcmp(sym, "idle") == 0) {
				idle = a;
				break;
			}
		if (f)
			fclose(f);
	}
	if (idle == -1) {
		t_skip(name, "no idle symbol");
		return;
	}
	sprintf(req, "busy %d %lx %lx", secs, idle, idle + 6);
	if ((r = host(req)) == 0 || sscanf(r, "%ld %ld", &b, &e) != 2)
		t_info(name, "no sample: %s", r ? r : "no reply");
	else
		t_info(name, "busy %ld of %d ms, last busy at %ld ms", b, secs * 1000, e);
}

/* N bytes at ADDR in guest P, through /proc; 0 on success */
static int
peek(p, addr, buf, n)
	pid_t p;
	unsigned long addr;
	char *buf;
	int n;
{
	char path[64];
	int fd, r;

	sprintf(path, "/proc/%05ld", (long)p);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	r = lseek(fd, (off_t)addr, SEEK_SET) == (off_t)addr && read(fd, buf, n) == n ? 0 : -1;
	close(fd);
	return r;
}

/*
 * After the drags, with the test's SYS: polls ended: the Workbench screen
 * is the display, nothing was copied, and the helpers sleep while the
 * guest idles.
 */
static void
idle(p)
	pid_t p;
{
	static struct mig_rtg r0, r1;
	struct mig_fs_status f0, f1;
	long dw, bw;

	settle("amiga_idle");
	if (peek(p, MIG_RTG_BASE, (char *)&r0, sizeof r0) < 0 ||
	    peek(p, MIG_FS_STATUS, (char *)&f0, sizeof f0) < 0) {
		t_check("direct_default", 0, "guest memory: %s", T_ERR);
		return;
	}
	nap(5000);
	if (peek(p, MIG_RTG_BASE, (char *)&r1, sizeof r1) < 0 ||
	    peek(p, MIG_FS_STATUS, (char *)&f1, sizeof f1) < 0) {
		t_check("idle_wakeups", 0, "guest memory: %s", T_ERR);
		return;
	}
	t_check("direct_default", !r1.copy && !r1.copies && r1.width == r1.max_width &&
	    r1.stride == r1.vstride, "copy %u, %u copies, screen %ux%u rows %u, display %u rows %u",
	    r1.copy, r1.copies, r1.width, r1.height, r1.stride, r1.max_width, r1.vstride);
	dw = r1.wakes - r0.wakes;
	bw = f1.wakes - f0.wakes;
	t_info("idle_wakeups", "in 5 s: display %ld, SYS: %ld", dw, bw);
	t_check("idle_wakeups", dw <= 2 && bw <= 2, "display %ld, SYS: %ld in 5 s", dw, bw);
}

/*
 * Drags a shell window's title bar 200,150 up and left and times the
 * screen settling after the button is released, with the CPU time the
 * guest (user, system) and its helpers used meanwhile.
 */
static void
drag(p)
	pid_t p;
{
	char b[64], *r, prev[16], cur[16];
	long t0, t1, t2, g0[2], g1[2], h0[2], h1[2], tv[4];
	int k, v[4];

	sprintf(b, "%s/draggo", SYS);
	close(creat(b, 0644));
	sleep(5);
	host("move -2000 -2000 50");
	host("move 400 204 50");
	settle("amiga_d0");
	cpu(p, 0, g0);
	cpu(p, 1, h0);
	traps(p, tv);
	t0 = t_now_ms();
	host("button 1");
	host("move -200 -150 50");
	host("button 0 0");
	t1 = t2 = t_now_ms();
	busy(6, "drag_redraw");
	strcpy(prev, "amiga_d0");
	for (k = 0; k < 40; k++) {
		sprintf(cur, "amiga_d%d", k + 1);
		sprintf(b, "shot %s", cur);
		host(b);
		sprintf(b, "cmp %s %s", prev, cur);
		if (k && (r = host(b)) != 0 && strcmp(r, "same") == 0)
			break;
		t2 = t_now_ms();
		strcpy(prev, cur);
	}
	cpu(p, 0, g1);
	cpu(p, 1, h1);
	pbox("amiga_d0", prev, 0, v);
	t_info("drag_time", "press to release %ld ms, release to settled %ld ms", t1 - t0, t2 - t1);
	t_info("drag_cpu", "in %ld ms: guest user %ld sys %ld, helpers user %ld sys %ld",
	    t_now_ms() - t0, g1[0] - g0[0], g1[1] - g0[1], h1[0] - h0[0], h1[1] - h0[1]);
	trapinfo("drag_traps", p, tv);
	t_check("drag_moved", v[2] - v[0] >= 400 && v[3] - v[1] >= 250,
	    "changed %d,%d-%d,%d", v[0], v[1], v[2], v[3]);
	/* back down in 60 small moves 16 ms apart, as a hand drags */
	settle("amiga_g0");
	cpu(p, 0, g0);
	cpu(p, 1, h0);
	t0 = t_now_ms();
	host("prof 3 glide");
	host("button 1");
	host("glide 60 3 2 16");
	nap(1800);
	host("button 0 0");
	t1 = t2 = t_now_ms();
	busy(6, "glide_redraw");
	strcpy(prev, "amiga_g0");
	for (k = 0; k < 40; k++) {
		sprintf(cur, "amiga_g%d", k + 1);
		sprintf(b, "shot %s", cur);
		host(b);
		sprintf(b, "cmp %s %s", prev, cur);
		if (k && (r = host(b)) != 0 && strcmp(r, "same") == 0)
			break;
		t2 = t_now_ms();
		strcpy(prev, cur);
	}
	cpu(p, 0, g1);
	cpu(p, 1, h1);
	pbox("amiga_g0", prev, 0, v);
	t_info("glide_time", "press to release %ld ms, release to settled %ld ms", t1 - t0, t2 - t1);
	t_info("glide_cpu", "in %ld ms: guest user %ld sys %ld, helpers user %ld sys %ld",
	    t_now_ms() - t0, g1[0] - g0[0], g1[1] - g0[1], h1[0] - h0[0], h1[1] - h0[1]);
	t_check("glide_moved", v[2] - v[0] >= 400 && v[3] - v[1] >= 250,
	    "changed %d,%d-%d,%d", v[0], v[1], v[2], v[3]);
	/* a window over the whole screen, opened and closed by the guest */
	settle("amiga_v0");
	busy(3, "idle_busy");
	traps(p, tv);
	sprintf(b, "%s/covergo", SYS);
	close(creat(b, 0644));
	busy(10, "cover");
	trapinfo("cover_traps", p, tv);
	host("shot amiga_uncover");
}

/*
 * Tools > Log Out, Control-click being the menu button: the session's
 * exit ends startmig with status 0.  Tools is the last menu title and
 * Log Out the item above root's Shut Down; positions come from the
 * screen.  o is the screen's origin.  0 if startmig is still running.
 */
static int
logout(p, stp, o)
	pid_t p;
	int *stp, o[2];
{
	char b[48];
	int r = 1, x, y, h, v[4], m[4];

	setsym("guest_adcall", 0L);
	setsym("guest_adtest", 1L);		/* a Shut Down picked by mistake only records */
	/* a click on the Workbench window makes its menus current */
	host("move -2000 -2000 50");
	host("move 100 400 50");
	host("click 1");
	/* along the menu bar with the button held: the last menu to drop is Tools */
	host("move -2000 -2000 50");
	host("move 2 4 50");
	nap(500);
	host("shot amiga_m0");
	host("down ctrl");
	host("button 1");
	m[0] = m[1] = m[2] = m[3] = -1;
	for (x = 2, h = 0; x < 400; x += 12) {
		if (x > 2)
			host("move 12 0 50");
		nap(300);
		host("shot amiga_scan");
		if (!pbox("amiga_m0", "amiga_scan", o[1] + 24, v) || v[3] - v[1] < 10) {
			if (m[0] >= 0)
				break;
			continue;
		}
		if (v[0] != m[0]) {
			memcpy(m, v, sizeof m);
			h = x;
		}
	}
	if (x >= 400)
		x -= 12;
	sprintf(b, "move %d 0 50", h + 4 - x);
	host(b);
	x = h + 4;
	nap(800);
	host("shot amiga_menu");
	if (m[0] >= 0) {
		/*
		 * Down onto the bottom item: its highlight's top, against the
		 * menu's bottom, gives the item height.  Then one item up.
		 */
		y = m[3] - o[1] - 5;
		sprintf(b, "move 0 %d 50", y - 4);
		host(b);
		nap(500);
		host("shot amiga_last");
		h = pbox("amiga_menu", "amiga_last", o[1] + 24, v) ? m[3] - 2 - v[1] : 0;
		t_info("boot_logout", "Tools at %d, menu %d,%d-%d,%d, item height %d", x,
		    m[0] - o[0], m[1] - o[1], m[2] - o[0], m[3] - o[1], h);
		if (h >= 6 && h <= 24) {
			sprintf(b, "move 0 %d 50", v[1] - o[1] - h / 2 - y);
			host(b);
			nap(500);
			host("shot amiga_tools");
		} else
			host("move 0 -100 50");		/* off the menu: nothing picked */
	} else
		t_info("boot_logout", "no menu below the bar");
	host("button 0");
	host("up ctrl");
	if (t_waitchild(p, stp, 20) < 0)
		r = 0;
	else
		t_check("boot_logout", WIFEXITED(*stp) && WEXITSTATUS(*stp) == 0 &&
		    getsym("guest_adcall") == 0, "startmig status 0x%x, uadmin %#lx", *stp,
		    getsym("guest_adcall"));
	setsym("guest_adtest", 0L);
	setsym("guest_adcall", 0L);
	return r;
}


static void
boot()
{
	static char *mark[] = { "S/Startup-Sequence", "C/IPrefs", "C/LoadWB", "LIBS/workbench.library",
	    "LIBS/Picasso96/container.card", "DEVS/Monitors/Container.info", "LIBS/Picasso96/rtg.library",
	    "DEVS/Picasso96Settings", "Prefs/Env-Archive/Picasso96/DisableAmigaBlitter" };
	static char *name[] = { "boot_dos", "boot_cli", "boot_loadwb", "boot_workbench", "boot_rtg",
	    "boot_icon", "boot_rtglib", "boot_modes", "boot_env" };
	int i, k, n, st, fd, w, h, seen[9], b[4], o[4];
	char *r;
	char line[256];
	FILE *f;
	time_t t0;
	pid_t p;

	if ((fd = open(SYSDEV, O_RDONLY)) < 0) {
		t_skip("boot", "no SYS: disk attached");
		return;
	}
	close(fd);
	if (!t_check("boot_mount", system("/sbin/mount -F ufs " SYSDEV " " SYS) == 0,
	    "mount " SYSDEV " failed"))
		return;
	/* the most private memory this system reserves now, in 4 MB steps */
	{
		struct rlimit rl;

		if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_VMEM, &rl);
		}
	}
	for (k = 4; k <= 128; k += 4) {
		caddr_t m = mmap((caddr_t)0, (size_t)k << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		    fd = open("/dev/zero", O_RDWR), 0);
		close(fd);
		if (m == (caddr_t)-1)
			break;
		munmap(m, (size_t)k << 20);
	}
	t_info("boot_reserve", "%d MB of private memory can be reserved (%s)", k - 4, T_ERR);
	t0 = migt0 = time((time_t *)0);
	if ((p = fork()) == 0) {
		fd = open("/tmp/startmig.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv("HOME=/nonexistent");
		/* this root has no swap: fast RAM within what it can reserve */
		execl("/tests/amiga/startmig", "startmig", "--census", "-m", "16",
		    "-boot", "/etc/amiga/boot.rom", (char *)0);
		_exit(127);
	}
	if (p < 0) {
		t_check("boot_start", 0, "fork: %s", T_ERR);
		system("/sbin/umount " SYS);
		return;
	}
	/* where the boot spends its time: PC samples to LoadWB */
	host("prof 40 boot");
	memset(seen, 0, sizeof seen);
	for (i = 0; i < 180 && waitpid(p, &st, WNOHANG) != p; i++) {
		for (k = n = 0; k < 9; k++) {
			if (!seen[k] && used(mark[k], t0)) {
				seen[k] = 1;
				t_info(name[k], "SYS:%s read after %d s", mark[k], i);
			}
			n += seen[k];
		}
		if (n == 9)
			break;
		sleep(1);
	}
	for (k = 0; k < 5; k++)
		t_check(name[k], seen[k], "SYS:%s never read", mark[k]);
	t_check("boot_alive", i < 180 ? kill(p, 0) == 0 : 1, "startmig ended: status 0x%x", st);
	{
		long z[4];

		z[0] = z[1] = z[2] = 0;
		z[3] = t_now_ms() - (time((time_t *)0) - t0) * 1000L;
		trapinfo("boot_traps", p, z);
	}
	/* Workbench draws its screen well after LoadWB starts */
	w = h = 0;
	for (k = 0; k < 30; k++) {
		sleep(5);
		if ((r = host("shot amiga_wb")) == 0 || sscanf(r, "%d %d", &w, &h) != 2 ||
		    (r = host("lit amiga_wb")) == 0 || atoi(r) > w * h / 2)
			break;
	}
	if (r == 0) {
		t_skip("boot_screen", "no test host");
		bsdsock();
	} else {
		t_check("boot_screen", atoi(r) > 0, "screen black: no RTG screen");
		t_info("boot_screen", "%s of %d pixels lit %d s after the boot marks", r, w * h, 5 * k + 5);
		/* the backdrop fills most of the Workbench screen */
		t_check("boot_wbscreen", atoi(r) > w * h / 2, "%s of %d pixels lit", r, w * h);
		/*
		 * The mouse sums motion until polled: a reversal sent at once would
		 * cancel pending motion, so moves go in polled steps.  Positions are
		 * the box of pixels a move changed, below the title bar.
		 */
		host("move -2000 -2000 50");
		settle("amiga_home");
		host("move 200 160 50");
		track("amiga_home", "amiga_in0", 0, 100, o);
		/* the old pointer is at the origin: the box's top left */
		k = pbox("amiga_home", "amiga_in0", o[1] + 16, b);
		t_info("boot_input", "origin %d,%d; pointer at %d,%d", o[0], o[1], b[0] - o[0], b[1] - o[1]);
		t_check("boot_input", k && at(b[0] - o[0], 200) && at(b[1] - o[1], 160),
		    "pointer at %d,%d, not 200,160", b[0] - o[0], b[1] - o[1]);
		/* drag on the backdrop: the lasso runs from 200,160 to 260,200 */
		host("button 1");
		host("move 60 40 50");
		track("amiga_in0", "amiga_in1", o[1] + 16, 0, b);
		host("button 0");
		t_info("boot_drag", "lasso %d,%d-%d,%d", b[0] - o[0], b[1] - o[1], b[2] - o[0], b[3] - o[1]);
		t_check("boot_drag", at(b[0] - o[0], 200) && at(b[1] - o[1], 160) &&
		    near(b[2] - o[0], 260) && near(b[3] - o[1], 200),
		    "lasso %d,%d-%d,%d, not 200,160-260,200", b[0] - o[0], b[1] - o[1], b[2] - o[0], b[3] - o[1]);
		/* and back up and left */
		settle("amiga_in2");
		host("move -100 -80 50");
		track("amiga_in2", "amiga_back", o[1] + 16, 0, b);
		t_info("boot_back", "pointer at %d,%d", b[0] - o[0], b[1] - o[1]);
		t_check("boot_back", at(b[0] - o[0], 160) && at(b[1] - o[1], 120) &&
		    near(b[2] - o[0], 260) && near(b[3] - o[1], 200),
		    "pointer at %d,%d, not 160,120", b[0] - o[0], b[1] - o[1]);
		/* Left Amiga with the cursor keys moves the pointer */
		host("shot amiga_in2");
		for (k = 0; k < 15; k++) {
			host("key meta_l+shift+down");
			host("key meta_l+shift+right");
			host("shot amiga_key");
			if ((r = host("cmp amiga_in2 amiga_key")) == 0 || strncmp(r, "diff", 4) == 0)
				break;
		}
		t_check("boot_key", r && strncmp(r, "diff", 4) == 0, "screen unchanged by keys");
		/* before the timings */
		bsdsock();
		ibrowse();
		pointer(p);
		drag(p);
		sound();
		idle(p);
	}
	if (!w || !logout(p, &st, o)) {
		if (w)
			t_fail("boot_logout", "startmig still running");
		kill(p, SIGTERM);
		t_waitchild(p, &st, 10);
		t_check("boot_exit", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
		    "startmig status 0x%x", st);
	}

	if (w) {
		sleep(2);
		host("shot amiga_exit");
		r = host("cmp amiga_wb amiga_exit");
		t_check("boot_release", r && strncmp(r, "diff", 4) == 0, "session still shown: %s",
		    r ? r : "no reply");
	}
	/* the helpers drop their files when they see the exit */
	for (i = 0; i < 10 && (sleep(1), system("/sbin/umount " SYS) != 0); i++)
		;
	/* the display's input-to-screen times, a line per 2 s with input, and its pointer */
	if ((f = fopen("/tmp/startmig.0.log", "r")) != 0) {
		while (fgets(line, sizeof line, f) != 0)
			{
				fputs(line, stdout);
				fflush(stdout);
			}
		fclose(f);
	}
	/* line by line: the console takes at most 256 bytes per write */
	if ((f = fopen("/tmp/startmig.log", "r")) != 0) {
		while (fgets(line, sizeof line, f) != 0) {
			fputs(line, stdout);
			fflush(stdout);
		}
		fclose(f);
	}
	t_check("boot_umount", i < 10, "umount " SYS " failed");
}

main(argc, argv)
	int argc;
	char **argv;
{
	struct mod_mreg reg;
	struct amigainfo ai;
	struct stat sb;
	int mj = AMIGA_MAJOR;

	if (argc > 1 && strcmp(argv[1], "stat") == 0) {
		struct amigastat st;
		int fd = open("/dev/amiga", O_RDWR);

		/* after exec: a plain process again */
		exit(fd >= 0 && E(ioctl(fd, AMIGAIOC_STAT, &st)) == ENXIO ? 0 : 1);
	}
	t_init("amiga", 900);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/amigaguest", &sb) < 0 || stat("/dev/amiga", &sb) < 0 ||
	    stat(GUEST, &sb) < 0) {
		t_skip("all", "no amigaguest module or guest image on this root");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "amigaguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();
	if (!t_check("open", (afd = open("/dev/amiga", O_RDWR)) >= 0, "/dev/amiga: %s", T_ERR))
		return t_done();
	ioctls();
	{
		struct modstatus ms;
		int id = 1;

		while (modstat(id, &ms, 1) == 0) {
			printf("module %s base %lx size %x\n", ms.ms_name, (long)ms.ms_base, ms.ms_size);
			id = ms.ms_id + 1;
		}
		fflush(stdout);
	}
	if (afd < 0)
		return t_done();
	if (ioctl(afd, AMIGAIOC_INFO, &ai) < 0 || !(ai.ai_features & AMIGA_FEAT_EXPERIMENTAL)) {
		t_skip("guest", "no 68040 execution path");
		return t_done();
	}
	/* in a child: a fault in the card's code must not end the suite */
	{
		pid_t dp;
		int dst;

		fflush(stdout);
		if ((dp = fork()) == 0) {
			drawtest();
			fflush(stdout);
			_exit(0);
		}
		t_waitchild(dp, &dst, 60);
		t_check("draw_ran", dp > 0 && WIFEXITED(dst) && WEXITSTATUS(dst) == 0, "status 0x%x", dst);
	}
	guests();
	exits();
	cost();
	two();
	unloadbusy();
	if (stat(ROM, &sb) < 0) {
		t_skip("rom", "no Kickstart ROM on this root");
		return t_done();
	}
	rom();
	if (stat(SYSDEV, &sb) < 0 || stat("/tests/amiga/startmig", &sb) < 0) {
		t_skip("boot", "no SYS: volume on this root");
		return t_done();
	}
	boot();
	return t_done();
}
