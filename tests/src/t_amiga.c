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
 * and Workbench loading.
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
			if (read(go[0], &c, 1) != 1)
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

static void
boot()
{
	static char *mark[] = { "S/Startup-Sequence", "C/IPrefs", "C/LoadWB", "LIBS/workbench.library" };
	static char *name[] = { "boot_dos", "boot_cli", "boot_loadwb", "boot_workbench" };
	int i, k, st, fd, seen[4];
	char *r;
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
		for (k = 0; k < 4; k++)
			if (!seen[k] && used(mark[k], t0)) {
				seen[k] = 1;
				t_info(name[k], "SYS:%s read after %d s", mark[k], i);
			}
		if (seen[0] && seen[1] && seen[2] && seen[3])
			break;
		sleep(1);
	}
	for (k = 0; k < 4; k++)
		t_check(name[k], seen[k], "SYS:%s never read", mark[k]);
	t_check("boot_alive", i < 180 ? kill(p, 0) == 0 : 1, "startmig ended: status 0x%x", st);
	sleep(10);
	if ((r = host("shot amiga_wb")) == 0 || (r = host("lit amiga_wb")) == 0)
		t_skip("boot_screen", "no test host");
	else if (atoi(r) == 0)
		t_skip("boot_screen", "screen black: no RTG screen");
	else {
		t_check("boot_screen", 1, "");
		t_info("boot_screen", "%s pixels lit", r);
	}
	kill(p, SIGKILL);
	t_waitchild(p, &st, 10);
	system("cat /tmp/startmig.log");
	/* the helpers drop their files when they see the exit */
	for (i = 0; i < 10 && (sleep(1), system("/sbin/umount " SYS) != 0); i++)
		;
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
