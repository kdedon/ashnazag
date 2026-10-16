/*
 * t_next: the nextguest module (registered, loaded, opened), then the
 * user's kernel from the OPENSTEP CD on SCSI disk 6 (tests/next/mkvol.sh),
 * loaded and entered by the native host without a ROM: it must print its
 * version line on the host console, stop at its missing root by returning to
 * the monitor (a host call), and reach the board's id, interrupt and
 * timer registers.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termio.h>
#include "sys/mod.h"
#include "next/profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c6d0s0"
#define	MNT	"/next"
#define	ROOT	"/tmp/next"
#define	FULLROOT	MNT "/fs"
#define	LOG	"/tmp/next.log"
#define	SCR1	0x0200c000UL
#define	KBASE	0x04000000UL

static int dev = -1;
static int full;			/* the volume holds the whole CD tree */
static char *root = ROOT;

/* the host console log, whole */
static char logbuf[262144];

static void
logread()
{
	int fd, n = 0;

	if ((fd = open(LOG, O_RDONLY)) >= 0) {
		off_t end = lseek(fd, (off_t)0, 2);

		lseek(fd, end > (off_t)sizeof logbuf - 1 ? end - (off_t)sizeof logbuf + 1 : (off_t)0, 0);
		n = read(fd, logbuf, sizeof logbuf - 1);
		close(fd);
	}
	logbuf[n > 0 ? n : 0] = 0;
}

/* the last lines of the log, without the register census and host calls */
static void
logtail(hfs)
	int hfs;
{
	static char c[sizeof logbuf];
	char *p, *e, *l[100];
	int i, k = 0;

	strcpy(c, logbuf);
	for (p = c; *p; p = e) {
		e = strchr(p, 10);
		if (e)
			*e++ = 0;
		else
			e = p + strlen(p);
		if (!strstr(p, "census") && (hfs ? strstr(p, "hfs op") != 0 : !strstr(p, "hfs op")))
			l[k++ % 100] = p;
	}
	for (i = k > (hfs ? 40 : 100) ? k - (hfs ? 40 : 100) : 0; i < k; i++)
		t_info("log", "%s", l[i % 100]);
}

/* whether the host console log holds the text */
static int
logfind(text)
	char *text;
{
	return strstr(logbuf, text) != 0;
}


static int hfd = -1;		/* the host line, -1 none */
static int hseq;
static char hrep[128];

/* send a host request; the answer in hrep, 0 if none in 20 s */
static char *
host(req)
	char *req;
{
	char buf[160], line[256];
	struct pollfd p;
	int n, len = 0;
	long t0;

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	t0 = t_now_ms();
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		char *nl, *s;
		int seq;

		if (poll(&p, 1, 500) <= 0)
			continue;
		if ((n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		while ((nl = strchr(line, '\n')) != 0) {
			*nl = 0;
			s = strstr(line, "@@ok ");
			if (s && sscanf(s + 5, "%d", &seq) == 1 && seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
			len -= nl + 1 - line;
			memmove(line, nl + 1, len + 1);
		}
		if (len > 200)
			len = 0;
	}
	return 0;
}

static void
hostopen()
{
	struct termio t;

	if ((hfd = open("/dev/term/b", O_RDWR | O_NOCTTY)) < 0)
		return;
	if (ioctl(hfd, TCGETA, &t) == 0) {
		t.c_iflag = IGNCR;
		t.c_oflag = 0;
		t.c_lflag = ICANON;
		t.c_cflag |= CREAD | CLOCAL;
		ioctl(hfd, TCSETAF, &t);
	}
	if (host("ping") == 0 || strcmp(hrep, "pong") != 0) {
		close(hfd);
		hfd = -1;
	}
}

/* a screen dump of the NeXT surface, named in the results */
static void
shot(name)
	char *name;
{
	char req[40];

	sprintf(req, "shot %s", name);
	if (host(req))
		t_info("shot", "%s %s", name, hrep);
}

/* a file of the root folder */
static char *
rootfile(name)
	char *name;
{
	static char path[64];

	sprintf(path, "%s/%s", root, name);
	return path;
}

/* bytes of the screen memory that are not zero */
static int
vramset()
{
	static unsigned char b[NX_VRAM_SIZE];
	int fd, n = 0, i;

	if ((fd = open(rootfile("vram"), O_RDONLY)) < 0)
		return -1;
	i = read(fd, b, sizeof b);
	close(fd);
	while (i-- > 0)
		n += b[i] != 0;
	return n;
}

/* the kernel loaded and entered without the ROM: it runs and probes the board */
static void
kernel()
{
	static struct nx_info info, now;
	pid_t pid;
	int i, k, gone = 0, status, id = 0, ints = 0, banner;

	if (access(DEV, 0) != 0 || system("/sbin/mount -F ufs " DEV " " MNT) != 0) {
		t_skip("kernel", "no NeXT volume");
		return;
	}
	full = access(FULLROOT "/NextApps", 0) == 0;
	if (full)
		root = FULLROOT;
	if (access(MNT "/native", X_OK) != 0 || access(full ? FULLROOT "/mach_kernel" : MNT "/mach_kernel", R_OK) != 0) {
		t_skip("kernel", "no native or kernel on the volume");
		system("/sbin/umount " MNT);
		return;
	}
	if (!full) {
		mkdir(ROOT, 0700);
		system("cp -r " MNT "/fs/etc " MNT "/fs/usr " MNT "/fs/bin " ROOT "; cp " MNT "/fs/.meta " ROOT ".meta");
	}
	unlink(rootfile("vram"));
	unlink(rootfile("nvram"));
	unlink(LOG);
	/* device nodes: the console, the controlling tty and null (raw = major << 8 | minor) */
	if (!full)
		system("mkdir -p " ROOT "/dev; cd " ROOT "/dev; echo 'mode=020622 raw=00000000' > console.__special__;"
		    " echo 'mode=020666 raw=00000200' > tty.__special__; echo 'mode=020666 raw=00000302' > null.__special__");
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		setpgrp();
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(fd, 2);
		putenv(full ? "NATIVEHOST_ROOT=" FULLROOT : "NATIVEHOST_ROOT=" ROOT);
		putenv("NEXT_CENSUS=1");
		execl(MNT "/native", "native", "-k", full ? FULLROOT "/mach_kernel" : MNT "/mach_kernel", (char *)0);
		_exit(127);
	}
	/* the module forgets the machine when it ends: keep the last look */
	for (i = 0; i < (full ? 1800 : 200) && !gone; i++) {
		poll((struct pollfd *)0, 0, 100);
		if (ioctl(dev, NX_INFO, &now) == 0 && now.ncensus >= info.ncensus)
			info = now;
		if (full ? i % 300 == 299 : i == 60 || i == 190) {
			char name[20];

			sprintf(name, "next_%d", i + 1);
			shot(name);
		}
		gone = waitpid(pid, &status, WNOHANG) != 0;
		if (gone) t_info("exit", "status %x", status);
	}
	logread();
	logtail(0);
	logtail(1);
	t_info("kernel", "gone %d ticks %lu io %lu last %lx pc %lx", gone, info.ticks, info.io,
	    info.lasta, info.lastpc);
	t_info("mmu", "fills %lu faults %lu space switches %lu flushes %lu, privileged %lu, ticks %lu",
	    info.mmu[0], info.mmu[1], info.mmu[2], info.mmu[3], info.mmu[4], info.ticks);
	for (i = 0; i < (int)info.ncensus && i < NX_NCENSUS; i++) {
		struct nx_census *c = &info.census[i];

		if (c->pc < KBASE || c->pc >= KBASE + 0x1000000UL)
			continue;
		id |= c->addr == SCR1 && !c->write;
		ints |= c->addr == 0x02007800UL || c->addr == 0x02007000UL;
	}
	t_check("km_on_screen", vramset() > 1000, "the kernel's console drew nothing on the screen");
	banner = logfind("NeXT Mach ");
	t_check("kernel_banner", banner, "no version line from the kernel's console");
	t_check("root_installed", logfind("next: root filesystem installed"), "the host filesystem was not installed");
	t_check("root_not_network", !logfind("no suitable root"), "the root probe stopped the kernel");
	t_check("root_mounted", logfind("root mounted from the host folder"), "the kernel did not mount the host folder as root");
	t_check("init_exec", !logfind("Load of /etc/mach_init") && logfind("hfs op 5 "),
	    "mach_init was not read and started as the first user task");
	t_info("init", "user mode: space switches %lu, fills %lu", info.mmu[2], info.mmu[0]);
	if (!full)
		t_check("rc_boot", logfind("error 0 result 0 sh\n") && logfind("error 0 result 0 rc.boot\n"),
		    "init did not start the shell on /etc/rc.boot");
	t_check("kernel_stays_up", !logfind("panic:"), "the kernel panicked");
	if (full) {
		/* rc.boot's text (three lines and the date) is on the screen, and the first task keeps serving */
		t_check("rc_text_on_screen", vramset() >= 120150, "no rc.boot text on the screen: %d bytes set", vramset());
		t_check("first_task_runs", info.utraps > 3000, "only %lu user traps", info.utraps);
	} else
		t_check("rc_boot_done", logfind("error 2 result 0 hostconfig\n") && logfind("error 2 result 0 utmp\n"),
		    "init did not get past rc.boot to the utmp file");
	t_info("traps", "%lu user traps in %d ms", info.utraps, i * 100);
	for (k = 0; k < 16 && k < (int)info.utraps; k++) {
		unsigned long *t = info.utrap[((info.utraps >= 16 ? info.utraps : 0) + k) % 16];

		t_info("utrap", "vector %lu d0 %lx pc %lx", t[0], t[1], t[2]);
	}
	t_check("host_alive", !gone || !WIFSIGNALED(status), "the host died of signal %d", WTERMSIG(status));
	t_check("kernel_loaded", info.ncensus > 0, "no board access");
	t_check("reads_id", id, "no read of the machine id from the kernel");
	t_check("interrupt_regs", ints, "no interrupt register access from the kernel");
	t_info("vram", "%d bytes set", vramset());
	if (!gone)
		kill(-pid, SIGTERM);
	waitpid(pid, &status, 0);
	for (i = 0; i < 100 && kill(-pid, 0) == 0; i++)
		poll((struct pollfd *)0, 0, 100);
	unlink(rootfile("vram"));
	unlink(rootfile("nvram"));
	system("/sbin/umount " MNT);
	if (!full)
		system("rm -rf " ROOT " " ROOT ".meta");
	unlink(LOG);
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

int
main()
{
	struct mod_mreg reg;
	int mj = NX_MAJOR, id;

	t_init("next", 600);
	if (access(MD "/nextguest", 0) < 0 || access("/dev/nextguest", 0) < 0) {
		t_skip("module", "no nextguest module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "nextguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	dev = open("/dev/nextguest", O_RDWR);
	if (!t_check("open_autoload", dev >= 0, "%s", T_ERR))
		return t_done();
	hostopen();
	kernel();
	close(dev);
	id = modid("nextguest");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
