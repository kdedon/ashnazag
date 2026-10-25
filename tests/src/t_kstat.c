/*
 * t_kstat -- /dev/kstat and libkstat: a sane snapshot, load and user time
 * rising under a busy loop, and top's batch display.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <stropts.h>
#include "sys/mod.h"
#include "kstat.h"
#include "t.h"

#define TOP	"/tests/top"
#define TOPOUT	"/tmp/top.out"

static unsigned long
val(kc, n)
	kstat_ctl_t *kc;
	char *n;
{
	return kstat_value(kc, n);
}

static void
snapshot(kc)
	kstat_ctl_t *kc;
{
	struct kstat_hdr h;
	char b[sizeof h + 64 * sizeof(kstat_named_t)];
	unsigned long t0, c0, c1, ps;
	int fd, n;

	t_check("magic", kc->kc_hdr.ks_magic == KSTAT_MAGIC, "%lx", kc->kc_hdr.ks_magic);
	t_check("records", kc->kc_hdr.ks_ndata >= 30, "%lu", kc->kc_hdr.ks_ndata);
	t_check("lookup_absent", kstat_data_lookup(kc, "no_such_record") == 0, "found");
	ps = val(kc, "pagesize");
	t_check("pagesize", ps >= 1024 && (ps & (ps - 1)) == 0, "%lu", ps);
	t_check("memory", val(kc, "physmem") > 0 && val(kc, "freemem") > 0 &&
	    val(kc, "freemem") <= val(kc, "physmem"), "physmem %lu freemem %lu",
	    val(kc, "physmem"), val(kc, "freemem"));
	t_check("fscale", val(kc, "fscale") > 0, "0");
	t_check("boot_time", val(kc, "boot_time") > 700000000 &&
	    val(kc, "boot_time") <= (unsigned long)time((time_t *)0), "%lu", val(kc, "boot_time"));
	t0 = kc->kc_hdr.ks_snaptime;
	c0 = val(kc, "cpu_idle") + val(kc, "cpu_user") + val(kc, "cpu_kernel") + val(kc, "cpu_wait");
	sleep(2);
	kstat_read(kc);
	c1 = val(kc, "cpu_idle") + val(kc, "cpu_user") + val(kc, "cpu_kernel") + val(kc, "cpu_wait");
	t_check("snaptime", kc->kc_hdr.ks_snaptime > t0, "%lu then %lu", t0, kc->kc_hdr.ks_snaptime);
	/* every tick lands in one CPU state */
	n = kc->kc_hdr.ks_snaptime - t0;
	t_check("cpu_ticks", c1 - c0 >= n / 2 && c1 - c0 <= n * 2, "%lu ticks in %d", c1 - c0, n);
	t_info("snapshot", "%lu records, %luK of %luK free, hz %lu", kc->kc_hdr.ks_ndata,
	    val(kc, "freemem") * (ps / 1024), val(kc, "physmem") * (ps / 1024), kc->kc_hdr.ks_hz);

	/* a short read gets the header and the first records; writers are refused */
	fd = open("/dev/kstat", O_RDONLY);
	n = read(fd, b, sizeof h + 2 * sizeof(kstat_named_t));
	memcpy((char *)&h, b, sizeof h);
	t_check("short_read", n == sizeof h + 2 * sizeof(kstat_named_t) && h.ks_magic == KSTAT_MAGIC &&
	    strcmp(((kstat_named_t *)(b + sizeof h))->name, "avenrun_1min") == 0, "%d", n);
	n = read(fd, b, sizeof b);
	t_check("reread", n == sizeof h + h.ks_ndata * sizeof(kstat_named_t), "%d", n);
	close(fd);
	t_check("no_write", open("/dev/kstat", O_RDWR) < 0 && errno == EACCES, "%s", T_ERR);
}

/* one spinner for 25 s: the 1-minute load passes 0.25 and user time dominates */
static void
busy(kc)
	kstat_ctl_t *kc;
{
	unsigned long u0, u1, i0, i1, l0, l1, fs;
	pid_t pid;
	int st;

	l0 = val(kc, "avenrun_1min");
	u0 = val(kc, "cpu_user");
	i0 = val(kc, "cpu_idle");
	if ((pid = fork()) == 0) {
		for (;;)
			;
	}
	sleep(25);
	kstat_read(kc);
	l1 = val(kc, "avenrun_1min");
	u1 = val(kc, "cpu_user");
	i1 = val(kc, "cpu_idle");
	fs = val(kc, "fscale");
	t_check("load_rises", l1 * 4 >= fs, "%lu/100 then %lu/100",
	    l0 * 100 / fs, l1 * 100 / fs);
	t_check("user_time", u1 - u0 > i1 - i0, "user %lu idle %lu", u1 - u0, i1 - i0);
	t_check("nrun", val(kc, "nrun") >= 1, "%lu", val(kc, "nrun"));
	t_info("load", "%lu.%02lu idle, %lu.%02lu after 25 s busy", l0 / fs, l0 * 100 / fs % 100,
	    l1 / fs, l1 * 100 / fs % 100);

	/* top's batch display, while the spinner runs */
	unlink(TOPOUT);
	if (access(TOP, X_OK) == 0) {
		char cmd[128], line[256], need[16], last[24][128];
		int procs = 0, cpu = 0, mem = 0, me = 0, nl = 0, i;
		FILE *f;

		sprintf(cmd, "cd /tmp && TERM=dumb %s -b -d2 -s1 > %s 2>&1", TOP, TOPOUT);
		st = system(cmd);
		sprintf(need, "%5d ", (int)pid);
		/* the second display, whose CPU figures cover the last second */
		if ((f = fopen(TOPOUT, "r")) != 0) {
			while (fgets(line, sizeof line, f)) {
				if (strncmp(line, "load averages", 13) == 0)
					procs = cpu = mem = me = nl = 0;
				if (strstr(line, "processes:"))
					procs = 1;
				if (strstr(line, "CPU states:") && strstr(line, "user"))
					cpu = 1;
				if (strstr(line, "Memory:") && strstr(line, " free"))
					mem = 1;
				if (strncmp(line, need, strlen(need)) == 0 && strstr(line, "t_kstat") &&
				    !strstr(line, " 0.0%"))
					me = 1;
				line[strcspn(line, "\n")] = '\0';
				if (nl < 24 && line[0])
					strncpy(last[nl++], line, sizeof last[0] - 1)[sizeof last[0] - 1] = '\0';
			}
			fclose(f);
		}
		for (i = 0; i < nl; i++)
			t_info("top", "%s", last[i]);
		t_check("top", st == 0 && procs && cpu && mem && me,
		    "exit %d, processes %d, cpu %d, memory %d, busy spinner %d", st, procs, cpu, mem, me);
	} else
		t_skip("top", "no %s", TOP);
	kill(pid, SIGKILL);
	t_waitchild(pid, &st, 10);
}

/* drain the master for ms milliseconds; 1 if pat came by */
static int
drain(m, ms, pat)
	int m, ms;
	char *pat;
{
	static char seen[4096];
	static int ns;
	struct pollfd pf;
	long end = t_now_ms() + ms;
	int n, hit = 0;

	ns = 0;
	while (t_now_ms() < end) {
		pf.fd = m;
		pf.events = POLLIN;
		if (poll(&pf, 1, 200) <= 0)
			continue;
		if ((n = read(m, seen + ns, sizeof seen - 1 - ns)) <= 0)
			break;
		ns += n;
		seen[ns] = '\0';
		if (strstr(seen, pat))
			hit = 1;
		if (ns > sizeof seen / 2) {
			memmove(seen, seen + ns - 64, 64);
			ns = 64;
		}
	}
	return hit;
}

/* top on a terminal: draws its screen and quits on q */
static void
interactive()
{
	extern char *ptsname();
	char *sname;
	int m, s, st = 0, shown;
	pid_t pid, me = getpid();

	if (access(TOP, X_OK) < 0 || (m = open("/dev/ptmx", O_RDWR)) < 0) {
		t_skip("top_tty", "no %s or pty", TOP);
		return;
	}
	grantpt(m);
	if (getpid() != me)
		_exit(1);
	unlockpt(m);
	sname = ptsname(m);
	if ((pid = fork()) == 0) {
		setsid();
		if ((s = open(sname, O_RDWR)) < 0)
			_exit(126);
		ioctl(s, I_PUSH, "ptem");
		ioctl(s, I_PUSH, "ldterm");
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		close(m);
		execl(TOP, "top", "-i", "-s1", (char *)0);
		_exit(127);
	}
	shown = drain(m, 4000, "load averages");
	write(m, "q\n", 2);
	drain(m, 500, "");
	t_check("top_tty", shown && t_waitchild(pid, &st, 10) == pid && WIFEXITED(st) &&
	    WEXITSTATUS(st) == 0, "screen %d, status %x", shown, st);
	close(m);
}

int
main()
{
	struct mod_mreg reg;
	kstat_ctl_t *kc;
	int mj = KSTAT_MAJOR;

	t_init("kstat", 120);
	/* loaded on the first open */
	strcpy(reg.md_modname, "kstat");
	reg.md_typedata = (caddr_t)&mj;
	modadm(MOD_TY_CDEV, MOD_C_MREG, &reg);
	if ((kc = kstat_open()) == 0) {
		t_fail("open", "/dev/kstat: %s", T_ERR);
		return t_done();
	}
	t_pass("open");
	snapshot(kc);
	busy(kc);
	interactive();
	t_check("close", kstat_close(kc) == 0, "%s", T_ERR);
	return t_done();
}
