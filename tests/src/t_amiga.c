/*
 * t_amiga.c -- the Amiga container: amigaguest on a 68040.
 *
 * Registers amigaguest for character major 57 (modules in
 * /tests/aux/mod.d), loads and unloads it, and checks /dev/amiga's
 * ioctl validation.  Small guests (/tests/amiga/guest.bin, guest.s)
 * then run without Kickstart: enter and leave, SR and stack changes
 * through RTE, privilege and trap reflection, the interrupt mask with
 * VERTB, STOP, exit, exec, fork, two guests at once and unload while a
 * guest holds the module.  With the local A4000 ROM, Kickstart runs
 * unpatched in a guest with AMIGAF_CENSUS for 40 s; the kernel logs
 * each custom-chip, CIA and other I/O register it touched at exit
 * ("amiga census" lines) and the guest's last fault.
 * With SYS: on a ufs volume (SCSI disk 0), startmig boots it from
 * /amiga/sys; host-side access times show DOS reading Startup-Sequence
 * and Workbench loading.  The boot extension binds container.card
 * before any screen opens; the RTG session then shows Workbench, a
 * drag on its backdrop changes the screen, IBrowse (when installed)
 * shows a local page and SIGTERM ends the session.
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
#include "sys/mod.h"
#include "amigaio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	GUEST	"/tests/amiga/guest.bin"
#define	ROM	"/etc/amiga/kicka4000.rom"
#define	SYSDEV	"/dev/dsk/c0d0s0"
#define	SYS	"/amiga/sys"
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
	ae.ae_flags = 4;
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
	t0 = time((time_t *)0);
	if ((p = fork()) == 0) {
		fd = open("/tmp/startmig.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv("HOME=/nonexistent");
		execl("/tests/amiga/startmig", "startmig", "--census", "-boot", "/etc/amiga/boot.rom", (char *)0);
		_exit(127);
	}
	if (p < 0) {
		t_check("boot_start", 0, "fork: %s", T_ERR);
		system("/sbin/umount " SYS);
		return;
	}
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
	/* Workbench draws its screen well after LoadWB starts */
	w = h = 0;
	for (k = 0; k < 30; k++) {
		sleep(5);
		if ((r = host("shot amiga_wb")) == 0 || sscanf(r, "%d %d", &w, &h) != 2 ||
		    (r = host("lit amiga_wb")) == 0 || atoi(r) > w * h / 2)
			break;
	}
	if (r == 0)
		t_skip("boot_screen", "no test host");
	else {
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
		ibrowse();
	}
	kill(p, SIGTERM);
	t_waitchild(p, &st, 10);
	t_check("boot_exit", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "startmig status 0x%x", st);
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
	t_init("amiga", 500);
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
	if (afd < 0)
		return t_done();
	if (ioctl(afd, AMIGAIOC_INFO, &ai) < 0 || !(ai.ai_features & AMIGA_FEAT_EXPERIMENTAL)) {
		t_skip("guest", "no 68040 execution path");
		return t_done();
	}
	guests();
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
