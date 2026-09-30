/*
 * t_sys.c -- identity, limits, environment, /proc.
 */
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/systeminfo.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>
#include <sys/wait.h>
#include <ulimit.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

static long magic = 0x5a5aa5a5;

static void
test_ids()
{
	struct utsname u;
	char buf[257], *r;
	long v;

	t_check("uname", uname(&u) >= 0 && u.sysname[0] && u.release[0], "%s", T_ERR);
	t_info("uname", "%s %s %s %s %s", u.sysname, u.nodename, u.release, u.version, u.machine);
	t_check("uname_machine", strncmp(u.machine, "mac68k", 6) == 0, "machine '%s'", u.machine);
	v = sysinfo(SI_ARCHITECTURE, buf, sizeof buf);
	t_info("sysinfo_arch", "%ld '%s'", v, v > 0 ? buf : "");
	v = sysinfo(SI_HW_PROVIDER, buf, sizeof buf);
	t_info("sysinfo_provider", "%ld '%s'", v, v > 0 ? buf : "");

	r = getenv("T_RUNNER");
	if (r != 0)
		t_check("getppid", getppid() == atol(r), "getppid %ld, runner %s", (long)getppid(), r);
	t_check("getpid", getpid() > 1, "pid %ld", (long)getpid());
	t_check("uid_root", getuid() == 0 && geteuid() == 0, "uid %d euid %d", (int)getuid(), (int)geteuid());
	t_info("pgrp_sid", "pgrp %ld sid %ld", (long)getpgrp(), (long)getsid(0));
}

static void
test_limits()
{
	struct rlimit rl;
	long v;
	int fds[300], n, i;

	v = ulimit(UL_GETFSIZE);
	t_check("ulimit_fsize", v > 0, "ulimit %ld", v);
	v = ulimit(UL_GMEMLIM);
	t_info("ulimit_brk", "0x%lx", v);
	t_check("getrlimit_nofile", getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0, "%s", T_ERR);
	t_info("rlimit_nofile", "cur %ld max %ld", (long)rl.rlim_cur, (long)rl.rlim_max);
	n = 0;
	for (i = 0; i < 300; i++) {
		fds[i] = open("/dev/null", O_RDONLY);
		if (fds[i] < 0)
			break;
		n++;
	}
	t_check("open_until_EMFILE", i < 300 && errno == EMFILE && n + 3 >= (int)rl.rlim_cur - 3,
	    "opened %d, errno %d, limit %ld", n, errno, (long)rl.rlim_cur);
	while (--i >= 0)
		close(fds[i]);
	t_info("sysconf", "CLK_TCK %ld PAGESIZE %ld OPEN_MAX %ld CHILD_MAX %ld ARG_MAX %ld",
	    sysconf(_SC_CLK_TCK), sysconf(_SC_PAGESIZE), sysconf(_SC_OPEN_MAX),
	    sysconf(_SC_CHILD_MAX), sysconf(_SC_ARG_MAX));
	t_check("pagesize", sysconf(_SC_PAGESIZE) == 4096, "%ld", sysconf(_SC_PAGESIZE));
	t_check("umask", umask(027) >= 0 && umask(022) == 027, "umask not kept");
}

static void
test_env()
{
	pid_t pid;
	int st;
	static char big[3000];

	t_check("putenv_getenv", putenv("T_A=1") == 0 && getenv("T_A") != 0 && strcmp(getenv("T_A"), "1") == 0,
	    "getenv");
	memset(big, 'x', sizeof big - 1);
	memcpy(big, "T_BIG=", 6);
	putenv(big);
	pid = fork();
	if (pid == 0) {
		execl("/tests/xh_d0", "xh", "env", "T_BIG", big + 6, (char *)0);
		_exit(126);
	}
	t_waitchild(pid, &st, 20);
	t_check("env_3KB_through_exec", st == 0, "status 0x%x", st);
}

static void
test_proc()
{
	char path[64], buf[64];
	prpsinfo_t ps;
	prstatus_t pst;
	DIR *d;
	struct dirent *de;
	int fd, n, found = 0;
	long v = 0;

	d = opendir("/proc");
	if (d == 0) {
		t_fail("proc_opendir", "%s", T_ERR);
		return;
	}
	sprintf(buf, "%05ld", (long)getpid());
	n = 0;
	while ((de = readdir(d)) != 0) {
		if (strcmp(de->d_name, buf) == 0)
			found = 1;
		n++;
	}
	closedir(d);
	t_check("proc_readdir_self", found, "%s not among %d entries", buf, n);

	sprintf(path, "/proc/%s", buf);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		t_fail("proc_open_self", "%s: %s", path, T_ERR);
		return;
	}
	t_pass("proc_open_self");
	t_check("PIOCPSINFO", ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_pid == getpid() &&
	    ps.pr_ppid == getppid() && strcmp(ps.pr_fname, "t_sys") == 0,
	    "pid %ld ppid %ld fname '%s'", (long)ps.pr_pid, (long)ps.pr_ppid, ps.pr_fname);
	t_info("psinfo", "size %ld pages, rss %ld pages", ps.pr_size, ps.pr_rssize);
	t_check("PIOCSTATUS", ioctl(fd, PIOCSTATUS, &pst) == 0 && pst.pr_pid == getpid(), "%s", T_ERR);
	lseek(fd, (off_t)&magic, 0);
	n = read(fd, (char *)&v, sizeof v);
	t_check("proc_read_memory", n == sizeof v && v == magic, "read %d value 0x%lx", n, v);
	close(fd);

	fd = open("/proc/00001", O_RDONLY);
	if (fd >= 0) {
		t_check("proc_init_psinfo", ioctl(fd, PIOCPSINFO, &ps) == 0 && ps.pr_pid == 1,
		    "pid %ld fname '%s'", (long)ps.pr_pid, ps.pr_fname);
		close(fd);
	} else
		t_fail("proc_init_psinfo", "open /proc/00001: %s", T_ERR);
}

static void
test_kmem()
{
	long fm = t_kmem("freemem"), ar = t_kmem("availrmem"), as = t_kmem("availsmem");

	if (fm == -1) {
		t_skip("kmem_freemem", "no /tests/ksyms or /dev/kmem");
		return;
	}
	t_info("kmem", "freemem %ld availrmem %ld availsmem %ld pages", fm, ar, as);
	t_check("kmem_freemem", fm > 0 && fm < 0x100000, "freemem %ld", fm);
	/* anonymous memory is reserved against swap: ani_max, ani_free, ani_resv */
	t_info("anoninfo", "swap %ld free %ld reserved %ld pages", t_kmem("anoninfo"),
	    t_kmem("anoninfo+4"), t_kmem("anoninfo+8"));
}

/* SR interrupt and SRQ scan rates with no input */
static void
test_adb()
{
	long i0 = t_kmem("adb_nintr"), s0 = t_kmem("adb_nsrq");

	if (i0 == -1) {
		t_skip("adb_idle", "no adb_nintr");
		return;
	}
	sleep(4);
	t_info("adb_idle", "%ld SR interrupts/s, %ld SRQ scans/s",
	    (t_kmem("adb_nintr") - i0) / 4, (t_kmem("adb_nsrq") - s0) / 4);
}

int
main()
{
	t_init("sys", 55);
	test_ids();
	test_limits();
	test_env();
	test_proc();
	test_kmem();
	test_adb();
	return t_done();
}
