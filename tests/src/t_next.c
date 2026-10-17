/*
 * t_next: the nextguest module (registered, loaded, opened), then the
 * user's kernel from the OPENSTEP CD on SCSI disk 6 (tests/next/mkvol.sh),
 * loaded and entered by the native host without a ROM: it must print its
 * version line on the host console, stop at its missing root by returning to
 * the monitor (a host call), with its board routines made host calls and
 * no access to an address nothing backs once it runs.
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
#include <time.h>
#include "sys/mod.h"
#include "next/profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	DEV	"/dev/dsk/c6d0s0"
#define	MNT	"/next"
#define	ROOT	"/tmp/next"
#define	FULLROOT	MNT "/fs"
#define	LOG	"/tmp/next.log"
#define	ERRLOG	"/tmp/next.err"

static int dev = -1;
static int full;			/* the volume holds the whole CD tree */
static char *root = ROOT;

/* the host console (the machine's text) and the launcher's own lines, kept apart so neither splits the other */
static char logbuf[1048576], errbuf[1048576];

static void
readlast(path, buf, size)
	char *path, *buf;
	int size;
{
	int fd, n = 0;

	if ((fd = open(path, O_RDONLY)) >= 0) {
		off_t end = lseek(fd, (off_t)0, 2);

		lseek(fd, end > (off_t)size - 1 ? end - (off_t)size + 1 : (off_t)0, 0);
		n = read(fd, buf, size - 1);
		close(fd);
	}
	buf[n > 0 ? n : 0] = 0;
}

static void
logread()
{
	readlast(LOG, logbuf, (int)sizeof logbuf);
	readlast(ERRLOG, errbuf, (int)sizeof errbuf);
}

/* the last lines of the log, without the register census and host calls; host calls without loops */
static void
logtail(hfs)
	int hfs;
{
	static char c[sizeof logbuf];
	char *p, *e, *l[100];
	int i, j, k = 0;

	strcpy(c, hfs ? errbuf : logbuf);
	for (p = c; *p; p = e) {
		e = strchr(p, 10);
		if (e)
			*e++ = 0;
		else
			e = p + strlen(p);
		for (j = 1; hfs && j <= 16 && j <= k && strcmp(l[(k - j) % 100], p); j++)
			;
		if (hfs && j <= 16 && j <= k)
			continue;
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
	return strstr(logbuf, text) != 0 || strstr(errbuf, text) != 0;
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

/* type a line on the host keyboard */
static void
typeline(text)
	char *text;
{
	char req[20];

	for (; *text; text++) {
		sprintf(req, *text == ' ' ? "key spc" : *text == '.' ? "key dot" : *text == '/' ? "key slash" :
		    *text == '-' ? "key minus" : "key %c", *text);
		host(req);
	}
	host("key ret");
}

/* shell prompts on the console so far */
static int
prompts()
{
	char *p;
	int n = 0;

	for (p = logbuf; (p = strstr(p, "# ")) != 0; p++)
		n++;
	return n;
}

/* a directory the shell stands in, removed and made a host link: the guest must not reach the host through it */
static void
escape()
{
	static char *cmd[] = { "mkdir /d", "cd /d", "rmdir /d", "ln -s /tmp /d", "mkdir next.esc", "ln -s /bin /b",
	    "/b/echo linkok", 0 };
	char **c;
	int n, i;

	for (c = cmd; *c; c++) {
		logread();
		n = prompts();
		typeline(*c);
		/* the next line once the shell prompts again: keys typed earlier would reach the command */
		for (i = 0; i < 150 && prompts() <= n; i++) {
			poll((struct pollfd *)0, 0, 100);
			logread();
		}
	}
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

/* hardware-page faults so far: accesses to addresses nothing backs */
static unsigned long
iofaults(in)
	struct nx_info *in;
{
	return in->io;
}

/* the kernel loaded and entered without the ROM: it runs and probes the board */
static void
kernel()
{
	static struct nx_info info, now;
	unsigned long io0 = 0;
	pid_t pid;
	int i, k, gone = 0, status, banner, typed = 0, after = 0;
	long t0 = t_now_ms(), login = 0;

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
	unlink(rootfile("d"));
	rmdir(rootfile("d"));
	unlink(rootfile("b"));
	rmdir("/tmp/next.esc");
	unlink(LOG);
	unlink(ERRLOG);
	/* device nodes: the console, the controlling tty and null (raw = major << 8 | minor) */
	if (!full)
		system("mkdir -p " ROOT "/dev; cd " ROOT "/dev; echo 'mode=020622 raw=00000000' > console.__special__;"
		    " echo 'mode=020666 raw=00000200' > tty.__special__; echo 'mode=020666 raw=00000302' > null.__special__");
	if ((pid = fork()) == 0) {
		int fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644), efd = open(ERRLOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		setpgrp();
		close(0);
		open("/dev/null", O_RDONLY);
		dup2(fd, 1);
		dup2(efd, 2);
		putenv(full ? "NATIVEHOST_ROOT=" FULLROOT : "NATIVEHOST_ROOT=" ROOT);
		putenv("NEXT_CENSUS=1");
		execl(MNT "/native", "native", "-k", full ? FULLROOT "/mach_kernel" : MNT "/mach_kernel", (char *)0);
		_exit(127);
	}
	/* the module forgets the machine when it ends: keep the last look */
	for (i = 0; i < (full ? 12000 : 300) && !gone && after < 50; i++) {
		after += typed == (full ? 3 : 2);
		poll((struct pollfd *)0, 0, 100);
		/* at the console's login or shell prompt, type a name, then a command */
		if (typed < 3 && i % 20 == 19) {
			logread();
			if (!typed && strstr(logbuf, "login: ")) {
				login = (t_now_ms() - t0) / 1000;
				typeline("root");
				typed = 1;
			} else if (typed < 2 && (typed ? strchr(strstr(logbuf, "login: "), '#') != 0 : logfind("\n#"))) {
				typeline("echo kbd");
				typed = 2;
			} else if (typed == 2 && full && logfind("\r\nkbd\r\n")) {
				escape();
				typed = 3;
			}
		}
		if (ioctl(dev, NX_INFO, &now) == 0 && now.ncensus >= info.ncensus)
			info = now;
		if (after == 1)
			io0 = iofaults(&info);
		if (full ? i % 1200 == 1199 : i == 60 || i == 190) {
			char name[20];

			sprintf(name, "next_%d", i + 1);
			shot(name);
			t_rearm(600);
		}
		gone = waitpid(pid, &status, WNOHANG) != 0;
		if (gone) t_info("exit", "status %x", status);
	}
	logread();
	logtail(0);
	logtail(1);
	t_info("kernel", "gone %d ticks %lu ints %lu io %lu last %lx pc %lx level %lu stat %lx mask %lx", gone, info.ticks,
	    info.ints, info.io, info.lasta, info.lastpc, info.level, info.stat, info.mask);
	t_info("mmu", "fills %lu faults %lu space switches %lu flushes %lu, privileged %lu, ticks %lu",
	    info.mmu[0], info.mmu[1], info.mmu[2], info.mmu[3], info.mmu[4], info.ticks);
	for (i = 0; i < (int)info.ncensus && i < NX_NCENSUS; i++)
		t_info("io", "%s %lx x%lu pc %lx last pc %lx", info.census[i].write ? "write" : "read",
		    info.census[i].addr, info.census[i].count, info.census[i].pc, info.census[i].lastpc);
	t_info("hcalls", "hfs %lu us %lu delay %lu timer %lu soft %lu time %lu nvram %lu/%lu", info.hcalls[1],
	    info.hcalls[3], info.hcalls[4], info.hcalls[5], info.hcalls[6], info.hcalls[7], info.hcalls[8], info.hcalls[9]);
	if (login)
		t_info("login", "prompt after %ld s", login);
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
	if (full)
		t_check("login_prompt", logfind("login: "), "no login prompt on the console");
	t_check("keys_reach_shell", logfind("echo kbd\r\n"), "typed keys did not reach the console's shell");
	t_check("keys_run", logfind("\r\nkbd\r\n"), "the typed command did not run");
	if (typed == 3) {
		t_check("host_confined", access("/tmp/next.esc", 0) != 0, "a guest directory made a host link reached the host");
		t_check("guest_symlink", logfind("\r\nlinkok\r\n"), "a link made in the guest did not lead to its target");
	}
	if (after > 1)
		t_check("io_steady", iofaults(&info) == io0, "%lu hardware-page faults in the %d s after the command",
		    iofaults(&info) - io0, (after - 1) / 10);
	if (full) {
		/* rc.boot's text with today's date, then init starts /etc/rc */
		time_t now = time((time_t *)0);
		char year[8];

		/* the console ends its lines with a return */
		sprintf(year, " %d\r\n", localtime(&now)->tm_year + 1900);
		t_check("rc_boot_text", logfind("Setting up root mount entries"), "no rc.boot text on the console");
		t_check("clock", logfind(year), "the date rc.boot printed is not this year");
		t_check("rc_runs", logfind("error 0 result 0 rc\n"), "init did not start /etc/rc");
	} else
		t_check("rc_boot_done", logfind("error 2 result 0 hostconfig\n") && logfind("error 2 result 0 utmp\n"),
		    "init did not get past rc.boot to the utmp file");
	t_info("traps", "%lu user traps in %d ms", info.utraps, i * 100);
	for (k = 0; k < 16 && k < (int)info.utraps; k++) {
		unsigned long *t = info.utrap[((info.utraps >= 16 ? info.utraps : 0) + k) % 16];

		t_info("utrap", "vector %lu d0 %lx pc %lx urp %lx d5 %lx calls %lx %lx %lx", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
	}
	t_check("host_alive", !gone || !WIFSIGNALED(status), "the host died of signal %d", WTERMSIG(status));
	t_check("host_services", logfind("next: the board's routines are host calls"), "a board routine was not patched");
	t_check("timer_ints", info.ints > 0 && info.hcalls[5] > 0, "no timer interrupts");
	t_info("vram", "%d bytes set", vramset());
	if (!gone)
		kill(-pid, SIGTERM);
	waitpid(pid, &status, 0);
	for (i = 0; i < 100 && kill(-pid, 0) == 0; i++)
		poll((struct pollfd *)0, 0, 100);
	unlink(rootfile("vram"));
	unlink(rootfile("nvram"));
	unlink(rootfile("d"));
	rmdir(rootfile("d"));
	unlink(rootfile("b"));
	rmdir("/tmp/next.esc");
	system("/sbin/umount " MNT);
	if (!full)
		system("rm -rf " ROOT " " ROOT ".meta");
	unlink(LOG);
	unlink(ERRLOG);
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
