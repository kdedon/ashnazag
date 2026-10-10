/* CP/M runs through its own guest device and preserves host-backed drives. */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include "sys/mod.h"
#include "cpmio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	START	"/cpm/bin/startcpm"
#define	H	"/tmp/cpmh"
#define	E	H "/CPM/t"
#define	TD	H "/.cpm/tmp/t"
#define	UF	"/cpmu.txt"

static char out[32768];
static int nout, in, from, ofd = -1;
static pid_t pid;
static char *startprog = START;
static long active_swap;

static pid_t
start(fd0, fd1)
	int fd0, fd1;
{
	pid_t p;

	if ((p = fork()) == 0) {
		setpgid(0, 0);
		putenv("HOME=" H);
		dup2(fd0, 0);
		dup2(fd1, 1);
		dup2(fd1, 2);
		for (fd0 = 3; fd0 < 64; fd0++)	/* the input's write end: EOF */
			close(fd0);
		execl(startprog, "startcpm", "-e", "t", "-d", "C=" H "/c", (char *)0);
		_exit(126);
	}
	if (p > 0)
		setpgid(p, p);
	return p;
}

static void
closeout()
{
	if (ofd >= 0) {
		close(ofd);
		ofd = -1;
	}
}

/* a session on pipes: in is its input, out collects what it writes */
static int
boot()
{
	int i[2], o[2];

	closeout();
	pid = -1;
	if (pipe(i) < 0)
		return -1;
	if (pipe(o) < 0) {
		close(i[0]); close(i[1]);
		return -1;
	}
	pid = start(i[0], o[1]);
	close(i[0]);
	close(o[1]);
	in = i[1];
	ofd = o[0];
	nout = from = 0;
	out[0] = 0;
	if (pid < 0) {
		close(in);
		closeout();
		return -1;
	}
	return ofd;
}

/* output since the last command until s appears; 1 if it did */
static int
waitfor(fd, s, ms)
	int fd, ms;
	char *s;
{
	struct pollfd pf;
	long end = t_now_ms() + ms;
	int k;

	pf.fd = fd;
	pf.events = POLLIN;
	while (strstr(out + from, s) == 0) {
		if (t_now_ms() > end || nout >= sizeof out - 1)
			return 0;
		if (poll(&pf, 1, 1000) <= 0)
			continue;
		if ((k = read(fd, out + nout, sizeof out - 1 - nout)) <= 0)
			return 0;
		nout += k;
		out[nout] = 0;
	}
	return 1;
}

/* a command line; its output, up to the next prompt, starts at out + from */
static int
cmd(fd, line, name)
	int fd;
	char *line, *name;
{
	char b[128];

	from = nout;
	sprintf(b, "%s\r", line);
	write(in, b, strlen(b));
	if (waitfor(fd, "\nA>", 60000))
		return 1;
	t_info(name, "no prompt after %s; output \"%.400s\"", line, out + from);
	return 0;
}

static void
put(path, s, n)
	char *path, *s;
	int n;
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		write(fd, s, n);
		close(fd);
	}
}

static void
rmtree(path)
	char *path;
{
	char sub[400];
	struct dirent *de;
	struct stat sb;
	DIR *dp;

	if (lstat(path, &sb) < 0)
		return;
	if ((sb.st_mode & S_IFMT) != S_IFDIR) {
		unlink(path);
		return;
	}
	if ((dp = opendir(path)) != 0) {
		while ((de = readdir(dp)) != 0)
			if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
				sprintf(sub, "%s/%.200s", path, de->d_name);
				rmtree(sub);
			}
		closedir(dp);
	}
	rmdir(path);
}

static void
clean()
{
	rmtree(H);
	unlink(UF);
}

static void
second()
{
	int fd, st = -1;
	pid_t p;

	fd = open("/dev/null", O_RDWR);
	if ((p = start(fd, fd)) > 0 && t_waitchild(p, &st, 30) < 0)
		st = -1;
	close(fd);
	t_check("one_session", WIFEXITED(st) && WEXITSTATUS(st) == 1, "status %#x", st);
}

static void
copy(from, to)
	char *from, *to;
{
	char buf[16384];
	int fd, n = -1;

	if ((fd = open(from, O_RDONLY)) >= 0) {
		n = read(fd, buf, sizeof buf);
		close(fd);
	}
	if (n > 0)
		put(to, buf, n);
}

/* a command that runs long; its time in ms, or -1 */
static long
timed(line, want, name)
	char *line, *want, *name;
{
	char b[128];
	long t0 = t_now_ms();

	from = nout;
	sprintf(b, "%s\r", line);
	write(in, b, strlen(b));
	if (!waitfor(ofd, "\nA>", 900000) || !strstr(out + from, want)) {
		t_fail(name, "output \"%.400s\"", out + from);
		return -1;
	}
	t_pass(name);
	return t_now_ms() - t0;
}

/* CP/M-80 .COM and CP/M-86 .CMD programs through the shim */
static void
shim(ofd)
	int ofd;
{
	struct stat sb;
	long t80, t86;

	if (stat("/tests/cpm/shim/PIP.COM", &sb) < 0) {
		t_skip("shim", "no CP/M-80/86 programs on this root");
		return;
	}
	copy("/tests/cpm/shim/DUMP.COM", "/cpm/dist/DUMP.COM");
	copy("/tests/cpm/shim/PIP.COM", E "/b/PIP.COM");
	copy("/tests/cpm/shim/BENCH80.COM", E "/b/BENCH80.COM");
	copy("/tests/cpm/shim/PIP.CMD", H "/c/PIP.CMD");
	copy("/tests/cpm/shim/STAT.CMD", H "/c/STAT.CMD");
	copy("/tests/cpm/shim/BENCH86.CMD", H "/c/BENCH86.CMD");
	if (cmd(ofd, "DUMP B:README.TXT", "dump80"))
		t_check("dump80", strstr(out + from, "48 45 4C 4C 4F") != 0, "output \"%.400s\"", out + from);
	cmd(ofd, "B:PIP B:COPY80.TXT=B:README.TXT", "pip80");
	t_check("pip80", stat(E "/b/copy80.txt", &sb) == 0 && sb.st_size == 128,
	    "copy80.txt: %s, output \"%.400s\"", T_ERR, out + from);
	cmd(ofd, "C:PIP B:COPY86.TXT=B:README.TXT", "pip86");
	t_check("pip86", stat(E "/b/copy86.txt", &sb) == 0 && sb.st_size == 128,
	    "copy86.txt: %s, output \"%.400s\"", T_ERR, out + from);
	if (cmd(ofd, "C:STAT C:*.*", "stat86"))
		t_check("stat86", strstr(out + from, "Free Space") && strstr(out + from, "PIP") &&
		    !strstr(out + from, "Bad Directory"), "output \"%.400s\"", out + from);
	t_rearm(2400);
	t80 = timed("B:BENCH80", "Z80 BENCH DONE", "bench80");
	t86 = timed("C:BENCH86", "I86 BENCH DONE", "bench86");
	t_info("shim_time", "68k fast paths: BENCH80 (10.5M 8080 instructions) %ld ms, BENCH86 (7.9M 8086) %ld ms",
	    t80, t86);
	unlink("/cpm/dist/DUMP.COM");
	unlink(E "/b/copy80.txt");
	unlink(E "/b/copy86.txt");
}

/* the 68k fast paths against the C references, instruction by instruction */
static void
diffs()
{
	char line[200];
	FILE *fp;
	int st;

	if (access("/tests/cpm/zdiff", 0) < 0)
		return;
	t_rearm(2400);
	st = system("/tests/cpm/zdiff > /tmp/zdiff.out 2>&1");
	t_check("zdiff", st == 0, "status %#x", st);
	system("/tests/cpm/zdiff -t 10 >> /tmp/zdiff.out 2>&1");
	st = system("/tests/cpm/idiff >> /tmp/zdiff.out 2>&1");
	t_check("idiff", st == 0, "status %#x", st);
	system("/tests/cpm/idiff -t 10 >> /tmp/zdiff.out 2>&1");
	if ((fp = fopen("/tmp/zdiff.out", "r")) != 0) {
		while (fgets(line, sizeof line, fp) != 0) {
			line[strcspn(line, "\n")] = 0;
			t_info("zdiff", "%s", line);
		}
		fclose(fp);
	}
	unlink("/tmp/zdiff.out");
}

/* the benchmarks again with the C interpreters alone */
static void
cbench()
{
	long t80, t86;
	int st = -1;

	if (access("/tests/cpm/startcpm-c", 0) < 0 || access(H "/c/BENCH86.CMD", 0) < 0)
		return;
	startprog = "/tests/cpm/startcpm-c";
	if ((ofd = boot()) < 0 || !waitfor(ofd, "A>", 60000)) {
		t_fail("cbench", "no session: %.300s", out);
		if (pid > 0)
			kill(-pid, SIGKILL);
		return;
	}
	t_rearm(2400);
	t80 = timed("B:BENCH80", "Z80 BENCH DONE", "cbench80");
	t86 = timed("C:BENCH86", "I86 BENCH DONE", "cbench86");
	t_info("shim_time_c", "C interpreters: BENCH80 %ld ms, BENCH86 %ld ms", t80, t86);
	write(in, "EXIT\r", 5);
	if (t_waitchild(pid, &st, 30) < 0)
		kill(-pid, SIGKILL);
	close(in);
	closeout();
	startprog = START;
}

/* DIR's cost per directory entry, against the same output from TYPE */
#define	NDIR	200
static void
dirtime(ofd)
	int ofd;
{
	char path[64], text[4096];
	struct guest_lone_stat c0, c1, c2;
	struct stat sb;
	long t0, t1, t2, t3;
	int i, n, fd = open("/dev/cpm", O_RDWR);

	for (i = 0; i < NDIR; i++) {
		sprintf(path, E "/b/f%03d.dat", i);
		put(path, "X", 1);
	}
	memset(&c0, 0, sizeof c0);
	c1 = c2 = c0;
	ioctl(fd, CPMIOC_STAT, &c0);
	t0 = t_now_ms();
	cmd(ofd, "DIR B:", "dir_many");
	t1 = t_now_ms();
	ioctl(fd, CPMIOC_STAT, &c1);
	n = nout - from;
	memset(text, 'X', sizeof text);
	for (i = 1; i * 80 <= n && i * 80 < sizeof text; i++)
		memcpy(text + i * 80 - 2, "\r\n", 2);
	put(E "/b/out.txt", text, i * 80 - 80);
	t2 = t_now_ms();
	cmd(ofd, "TYPE B:OUT.TXT", "dir_many");
	t3 = t_now_ms();
	ioctl(fd, CPMIOC_STAT, &c2);
	close(fd);
	t_check("dir_many", strstr(out + from - n, "F199") != 0, "DIR output \"%.200s\"", out + from - n);
	t_info("dir_time", "DIR of %d files %ld ms, %lu traps (%d bytes); TYPE of the same bytes %ld ms, %lu traps",
	    NDIR, t1 - t0, c1.traps - c0.traps, n, t3 - t2, c2.traps - c1.traps);
	t0 = t_now_ms();
	for (i = 0; i < NDIR; i++) {
		sprintf(path, E "/b/f%03d.dat", i);
		stat(path, &sb);
	}
	t_info("dir_stat", "%d host stats %ld ms", NDIR, t_now_ms() - t0);
	for (i = 0; i < NDIR; i++) {
		sprintf(path, E "/b/f%03d.dat", i);
		unlink(path);
	}
	unlink(E "/b/out.txt");
}

static int
module(name, st)
char *name;
struct modstatus *st;
{
	int id = 1;
	while (modstat(id, st, 1) == 0) {
		if (!strcmp(st->ms_name, name))
			return st->ms_id;
		id = st->ms_id + 1;
	}
	return -1;
}

static void
running(name)
char *name;
{
	struct modstatus st;
	int fd, id = module("cpmguest", &st);
	struct guest_lone_stat counters;
	fd = open("/dev/cpm", O_RDWR);
	memset(&counters, 0, sizeof counters);
	t_check("active_stats", fd >= 0 && ioctl(fd, CPMIOC_STAT, &counters) == 0 && counters.entered > 0,
	    "CP/M entries %lu", counters.entered);
	if (fd >= 0) close(fd);
	t_check(name, id > 0 && moduload(id) < 0 && errno == EBUSY,
	    "active CP/M module id %d: %s", id, T_ERR);
	active_swap = t_kmem("availsmem");
	t_info("active_memory", "resident %ld, swap %ld", t_kmem("availrmem"), active_swap);
	t_check("tos_absent_running", module("tosguest", &st) < 0,
	    "tosguest loaded during CP/M");
}

static void
released(name)
char *name;
{
	struct modstatus st;
	int id = module("cpmguest", &st);
	t_check(name, id > 0 && moduload(id) == 0, "module id %d: %s", id, T_ERR);
	t_check("cpm_unloaded", module("cpmguest", &st) < 0, "CP/M module remains loaded");
	t_check("tos_absent_after", module("tosguest", &st) < 0, "tosguest loaded during CP/M");
}

static void
recovered(name, resident, swap, pages)
char *name;
long resident, swap, pages;
{
	char label[64];
	long r = t_kmem("availrmem"), s = t_kmem("availsmem");
	t_info(name, "resident %ld -> %ld, swap %ld -> %ld, active swap %ld",
	    resident, r, swap, s, active_swap);
	sprintf(label, "%s_ram_released", name);
	t_check(label, r >= resident, "resident %ld -> %ld", resident, r);
	sprintf(label, "%s_swap_released", name);
	t_check(label, s >= swap, "swap %ld -> %ld", swap, s);
	sprintf(label, "%s_vm_released", name);
	t_check(label, active_swap >= 0 && s - active_swap >= pages,
	    "recovered %ld pages, CP/M RAM requires %ld", s - active_swap, pages);
}

int
main()
{
	struct mod_mreg reg;
	struct stat sb;
	char buf[8192];
	int mj = CPM_MAJOR, fd, ofd, st, n, id;
	struct modstatus ms;
	struct guest_lone_stat counters;
	long budget, swap, resident_now, swap_now, pages, pagebytes;

	t_init("cpm", 600);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	if (stat(MD "/cpmguest", &sb) < 0 || stat("/dev/cpm", &sb) < 0 ||
	    stat(START, &sb) < 0 || stat("/cpm/sys/CPM.SYS", &sb) < 0) {
		t_skip("all", "no cpmguest module, startcpm or CP/M-68K on this root");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "cpmguest");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "modadm: %s", T_ERR))
		return t_done();

	id = module("tosguest", &ms);
	if (id > 0 && !t_check("unload_tos", moduload(id) == 0, "%s", T_ERR))
		return t_done();
	t_check("tos_initial_absent", module("tosguest", &ms) < 0, "tosguest loaded");
	if (access("/tests/cpm/no-tos", 0) == 0)
		t_check("tos_module_file_absent", access(MD "/tosguest", 0) < 0, "tosguest packaged");
	if (!t_check("cpm_load", modload("cpmguest") > 0, "%s", T_ERR))
		return t_done();
	budget = t_kmem("availrmem");
	swap = t_kmem("availsmem");
	t_check("ram_accounting", budget >= 0 && swap >= 0, "memory counters unavailable");
	t_info("cold_memory", "resident %ld, swap %ld", budget, swap);
	pagebytes = sysconf(_SC_PAGESIZE);
	if (!t_check("page_size", pagebytes == 2048 || pagebytes == 4096,
	    "page size %ld", pagebytes))
		return t_done();
	pages = 0x400000L / pagebytes;

	clean();
	mkdir(H, 0755);
	mkdir(H "/CPM", 0755);
	mkdir(E, 0755);
	mkdir(E "/b", 0755);
	mkdir(H "/c", 0755);
	mkdir(H "/.cpm", 0755);
	mkdir(H "/.cpm/tmp", 0755);
	mkdir(TD, 0755);
	put(TD "/stale.txt", "x", 1);
	put(H "/CPM/DRIVES", "# B: for the tests\nB=CPM/t/b\n", (int)strlen("# B: for the tests\nB=CPM/t/b\n"));
	put(H "/home.txt", "HELLO FROM H:\r\n\032", 16);
	put(H "/c/Makefile.linux", "LONG NAME\r\n\032", 12);
	put(UF, "HELLO FROM O:\r\n\032", 16);
	put(E "/b/README.TXT", "HELLO FROM B:\r\n\032", 16);
	if ((fd = open("/tests/cpm/hello.68k", O_RDONLY)) >= 0) {
		n = read(fd, buf, sizeof buf);
		close(fd);
		put(E "/b/HELLO.68K", buf, n);
	}

	if ((ofd = boot()) < 0) {
		t_fail("boot", "pipe or fork: %s", T_ERR);
		return t_done();
	}
	if (!t_check("boot", waitfor(ofd, "A>", 120000),
	    "output \"%.400s\"", out)) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 10);
		close(in);
		closeout();
		return t_done();
	}
	running("active_unload_busy");
	t_check("a_empty", stat(E "/STAT.68K", &sb) < 0 && stat(E "/stat.68k", &sb) < 0,
	    "programs copied into A:");
	t_check("m_private", stat(TD "/stale.txt", &sb) < 0 && stat(TD, &sb) == 0 &&
	    (sb.st_mode & 0777) == 0700, "M: mode %o, stale file %s", sb.st_mode & 0777,
	    access(TD "/stale.txt", 0) == 0 ? "kept" : "gone");
	if (cmd(ofd, "DIR P:", "dir"))
		t_check("dir", strstr(out + from, "STAT") && strstr(out + from, "PIP") &&
		    strstr(out + from, "EXIT") && strstr(out + from, "MOUNT"),
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "TYPE H:HOME.TXT", "drive_h"))
		t_check("drive_h", strstr(out + from, "HELLO FROM H:") != 0, "output \"%.400s\"", out + from);
	if (cmd(ofd, "TYPE O:CPMU.TXT", "drive_o"))
		t_check("drive_o", strstr(out + from, "HELLO FROM O:") != 0, "output \"%.400s\"", out + from);
	if (cmd(ofd, "DIR C:", "drive_c"))
		t_check("mangle", strstr(out + from, "MAKEFI~") && strstr(out + from, "LIN"),
		    "output \"%.400s\"", out + from);
	cmd(ofd, "PIP M:T.TXT=B:README.TXT", "drive_m");
	t_check("drive_m", stat(TD "/t.txt", &sb) == 0, "M:T.TXT: %s, output \"%.400s\"", T_ERR, out + from);
	cmd(ofd, "PIP P:X.TXT=B:README.TXT", "p_readonly");
	t_check("p_readonly", stat("/cpm/dist/X.TXT", &sb) < 0 && stat("/cpm/dist/x.txt", &sb) < 0,
	    "P: written: output \"%.400s\"", out + from);
	if (cmd(ofd, "MOUNT D: /TMP/CPMH/C", "mount") && cmd(ofd, "DIR D:", "mount"))
		t_check("mount", strstr(out + from, "MAKEFI~") != 0, "output \"%.400s\"", out + from);
	if (cmd(ofd, "MOUNT", "mount_list"))
		t_check("mount_list", strstr(out + from, "D: " H "/c") && strstr(out + from, "P: /cpm/dist (R/O)"),
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "TYPE B:README.TXT", "type"))
		t_check("type", strstr(out + from, "HELLO FROM B:") != 0,
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "B:HELLO", "run"))
		t_check("run", strstr(out + from, "HELLO FROM CP/M-68K") != 0,
		    "output \"%.400s\"", out + from);
	if (cmd(ofd, "STAT", "stat"))
		t_check("stat", strstr(out + from, "SPACE") != 0, "output \"%.400s\"", out + from);
	if (cmd(ofd, "PIP A:COPY.TXT=B:README.TXT", "pip"))
		t_check("pip", cmd(ofd, "TYPE COPY.TXT", "pip") &&
		    strstr(out + from, "HELLO FROM B:") != 0, "output \"%.400s\"", out + from);
	t_check("host_copy", stat(E "/copy.txt", &sb) == 0 && sb.st_size == 128,
	    "copy.txt: %s", T_ERR);
	put(E "/late.txt", "LATE FROM HOST\r\n\032", 17);
	if (cmd(ofd, "TYPE LATE.TXT", "late"))
		t_check("late", strstr(out + from, "LATE FROM HOST") != 0, "output \"%.400s\"", out + from);
	dirtime(ofd);
	shim(ofd);
	second();
	from = nout;
	write(in, "EXIT\r", 5);
	st = -1;
	if (t_waitchild(pid, &st, 30) < 0)
		st = -1;
	t_check("exit", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x, output \"%.300s\"",
	    st, out + from);
	close(in);
	closeout();
	released("exit_unload");
	resident_now = t_kmem("availrmem"); swap_now = t_kmem("availsmem");
	t_info("warmup_memory", "resident %ld -> %ld, swap %ld -> %ld, active swap %ld",
	    budget, resident_now, swap, swap_now, active_swap);
	t_check("warmup_vm_released", active_swap >= 0 && swap_now - active_swap >= pages,
	    "recovered %ld pages, CP/M RAM requires %ld", swap_now - active_swap, pages);
	/* The complete workload primes persistent kernel metadata and file caches. */
	budget = resident_now;
	swap = swap_now;

	if ((ofd = boot()) >= 0 && t_check("reboot", waitfor(ofd, "A>", 60000),
	    "output \"%.400s\"", out)) {
		running("reloaded_unload_busy");
		t_check("m_emptied", stat(TD "/t.txt", &sb) < 0, "M: kept T.TXT");
		if (cmd(ofd, "TYPE COPY.TXT", "kept"))
			t_check("kept", strstr(out + from, "HELLO FROM B:") != 0,
			    "output \"%.400s\"", out + from);
	} else {
		if (pid > 0) {
			kill(-pid, SIGKILL);
			t_waitchild(pid, &st, 30);
			close(in);
			closeout();
		} else t_fail("reboot", "pipe or fork: %s", T_ERR);
		return t_done();
	}
	write(in, "EXIT\r", 5);
	st = -1;
	if (pid > 0 && t_waitchild(pid, &st, 30) < 0) st = -1;
	t_check("repeated_exit", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
	if (pid > 0 && st == -1) {
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 30);
	}
	close(in);
	closeout();
	released("repeated_exit_unload");
	recovered("exit", budget, swap, pages);

	if ((ofd = boot()) >= 0 && t_check("eof_boot", waitfor(ofd, "A>", 60000),
	    "output \"%.300s\"", out)) {
		running("eof_active_busy");
		close(in);
		st = -1;
		if (t_waitchild(pid, &st, 30) < 0) st = -1;
		t_check("eof_ends", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %#x", st);
		if (st == -1) {
			kill(-pid, SIGKILL);
			t_waitchild(pid, &st, 30);
		}
		closeout();
		released("eof_unload");
		recovered("eof", budget, swap, pages);
	} else {
		if (pid > 0) {
			kill(-pid, SIGKILL);
			t_waitchild(pid, &st, 30);
			close(in);
			closeout();
		} else t_fail("eof_boot", "pipe or fork: %s", T_ERR);
	}
	if ((ofd = boot()) >= 0 && t_check("kill_boot", waitfor(ofd, "A>", 60000),
	    "output \"%.300s\"", out)) {
		running("kill_active_busy");
		kill(-pid, SIGKILL);
		st = -1;
		t_waitchild(pid, &st, 30);
		t_check("kill_status", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "status %#x", st);
		close(in);
		closeout();
		released("kill_unload");
		recovered("kill", budget, swap, pages);
	} else {
		if (pid > 0) {
			kill(-pid, SIGKILL);
			t_waitchild(pid, &st, 30);
			close(in);
			closeout();
		} else t_fail("kill_boot", "pipe or fork: %s", T_ERR);
	}
	memset(&counters, 0, sizeof counters);
	fd = open("/dev/cpm", O_RDWR);
	t_check("stats", fd >= 0 && ioctl(fd, CPMIOC_STAT, &counters) == 0,
	    "CP/M entries %lu", counters.entered);
	if (fd >= 0) close(fd);
	released("stats_unload");
	diffs();
	cbench();
	clean();
	return t_done();
}
