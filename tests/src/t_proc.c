/*
 * t_proc.c -- fork, exec, wait, exit status, vfork.
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
#include <dirent.h>
#include <poll.h>
#include "t.h"

extern char **environ;
pid_t vfork();

/* run argv with stdout/stderr to /dev/null; returns wait status or -1 */
static int
run(path, argv, secs)
char *path, **argv;
int secs;
{
	pid_t pid;
	int st, fd;

	pid = fork();
	if (pid == -1)
		return -1;
	if (pid == 0) {
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execv(path, argv);
		_exit(126);
	}
	if (t_waitchild(pid, &st, secs) != pid)
		return -1;
	return st;
}

static char *
stdesc(st)
int st;
{
	static char buf[64];

	if (st == -1)
		sprintf(buf, "wait failed/timeout");
	else if (WIFEXITED(st))
		sprintf(buf, "exit %d", WEXITSTATUS(st));
	else if (WIFSIGNALED(st))
		sprintf(buf, "signal %d", WTERMSIG(st));
	else
		sprintf(buf, "status 0x%x", st);
	return buf;
}

static void
test_fork_wait()
{
	pid_t pid, w;
	int st, codes[5], i, ok = 1;
	char why[128];

	codes[0] = 0; codes[1] = 1; codes[2] = 42; codes[3] = 127; codes[4] = 255;
	why[0] = 0;
	for (i = 0; i < 5; i++) {
		pid = fork();
		if (pid == 0)
			_exit(codes[i]);
		if (pid == -1) {
			sprintf(why, "fork: %s", T_ERR);
			ok = 0;
			break;
		}
		w = waitpid(pid, &st, 0);
		if (w != pid || !WIFEXITED(st) || WEXITSTATUS(st) != codes[i]) {
			sprintf(why, "code %d: waitpid %ld %s", codes[i], (long)w, stdesc(st));
			ok = 0;
		}
	}
	t_check("fork_exit_status", ok, "%s", why);

	pid = fork();
	if (pid == 0) {
		kill(getpid(), SIGTERM);
		pause();
		_exit(0);
	}
	w = waitpid(pid, &st, 0);
	t_check("wait_signaled", w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
	    "%s", stdesc(st));

	t_check("wait_any_ECHILD", wait(&st) == -1 && errno == ECHILD, "wait with no children: %s", T_ERR);
}

static void
test_ppid()
{
	pid_t pid, me = getpid();
	int p[2], st;
	long v = -1;

	pipe(p);
	pid = fork();
	if (pid == 0) {
		v = getppid();
		write(p[1], (char *)&v, sizeof v);
		_exit(0);
	}
	read(p[0], (char *)&v, sizeof v);
	waitpid(pid, &st, 0);
	t_check("getppid_child", v == (long)me, "child getppid %ld, parent %ld", v, (long)me);

	/* orphan: grandchild is reparented to init */
	pid = fork();
	if (pid == 0) {
		if (fork() == 0) {
			int i;
			for (i = 0; i < 40 && getppid() != 1; i++)
				poll((void *)0, 0, 50);
			v = getppid();
			write(p[1], (char *)&v, sizeof v);
			_exit(0);
		}
		_exit(0);
	}
	waitpid(pid, &st, 0);
	v = -1;
	read(p[0], (char *)&v, sizeof v);
	t_check("orphan_reparent", v == 1, "orphan getppid %ld", v);
	close(p[0]);
	close(p[1]);
}

static void
test_many_forks()
{
	pid_t pids[25];
	int i, st, n, bad = 0, errs = 0;

	for (i = 0; i < 300; i++) {
		pid_t pid = fork();
		if (pid == 0)
			_exit(i & 0x7f);
		if (pid == -1) {
			errs++;
			continue;
		}
		if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != (i & 0x7f))
			bad++;
	}
	t_check("fork_300_sequential", bad == 0 && errs == 0, "%d bad status, %d fork errors", bad, errs);

	n = 0;
	for (i = 0; i < 25; i++) {
		pids[i] = fork();
		if (pids[i] == 0) {
			poll((void *)0, 0, 200);
			_exit(i);
		}
		if (pids[i] > 0)
			n++;
	}
	bad = 0;
	for (i = 0; i < 25; i++)
		if (pids[i] > 0 && (t_waitchild(pids[i], &st, 20) != pids[i] ||
		    !WIFEXITED(st) || WEXITSTATUS(st) != i))
			bad++;
	t_check("fork_25_concurrent", n == 25 && bad == 0, "%d forked, %d bad", n, bad);
}

static void
test_vfork()
{
	pid_t pid;
	int st;
	volatile int x = 1;

	pid = vfork();
	if (pid == 0) {
		x = 2;	/* shared with the parent under a real vfork */
		_exit(5);
	}
	if (pid == -1) {
		t_fail("vfork_exit", "vfork: %s", T_ERR);
		return;
	}
	waitpid(pid, &st, 0);
	t_check("vfork_exit", WIFEXITED(st) && WEXITSTATUS(st) == 5, "%s", stdesc(st));
	t_info("vfork_shared", "child store %s visible in parent", x == 2 ? "is" : "is not");

	pid = vfork();
	if (pid == 0) {
		execl("/tests/xh_d0", "xh", "exit", "9", (char *)0);
		_exit(126);
	}
	waitpid(pid, &st, 0);
	t_check("vfork_exec", WIFEXITED(st) && WEXITSTATUS(st) == 9, "%s", stdesc(st));
}

/* exec every /tests/xh_* image; each checks its own data and bss */
static void
test_exec_sizes()
{
	DIR *d;
	struct dirent *de;
	struct stat sb;
	char path[64], name[64], *av[2];
	int st, n = 0;

	d = opendir("/tests");
	if (d == 0) {
		t_fail("exec_sizes", "opendir /tests: %s", T_ERR);
		return;
	}
	while ((de = readdir(d)) != 0) {
		if (strncmp(de->d_name, "xh_", 3) != 0)
			continue;
		sprintf(path, "/tests/%s", de->d_name);
		stat(path, &sb);
		av[0] = path;
		av[1] = 0;
		st = run(path, av, 20);
		sprintf(name, "exec_%s", de->d_name);
		t_check(name, st == 0, "size %ld (mod 4096 = %ld): %s%s", (long)sb.st_size,
		    (long)(sb.st_size % 4096), stdesc(st),
		    st == 0x200 ? " (initialized data wrong)" :
		    st == 0x300 ? " (bss not zero)" : "");
		n++;
	}
	closedir(d);
	if (n == 0)
		t_fail("exec_sizes", "no /tests/xh_* found");
}

/* the root's own dynamically linked tools */
static void
test_exec_system()
{
	static char *cmds[][5] = {
		{ "/usr/bin/true", 0 },
		{ "/usr/bin/echo", "hello", 0 },
		{ "/usr/bin/pwd", 0 },
		{ "/usr/bin/uname", "-a", 0 },
		{ "/usr/bin/id", 0 },
		{ "/usr/bin/date", 0 },
		{ "/usr/bin/ls", "-l", "/tests", 0 },
		{ "/usr/bin/env", 0 },
		{ "/usr/bin/cat", "/etc/passwd", 0 },
		{ "/usr/bin/od", "-c", "/etc/group", 0 },
		{ "/usr/bin/wc", "/etc/passwd", 0 },
		{ "/usr/bin/head", "/etc/passwd", 0 },
		{ "/usr/bin/tail", "/etc/passwd", 0 },
		{ "/usr/bin/sed", "-n", "1p", "/etc/passwd", 0 },
		{ "/usr/bin/ps", "-ef", 0 },
		{ "/usr/bin/sleep", "0", 0 },
		{ "/sbin/sh", "-c", "exit 0", 0 },
		{ "/sbin/uname", "-m", 0 },
		{ 0 }
	};
	struct stat sb;
	char name[64], *b;
	int i, st;

	for (i = 0; cmds[i][0] != 0; i++) {
		b = strrchr(cmds[i][0], '/') + 1;
		sprintf(name, "exec_sys_%s%s", strncmp(cmds[i][0], "/sbin/", 6) ? "" : "sbin_", b);
		if (stat(cmds[i][0], &sb) == -1) {
			t_skip(name, "%s not in root", cmds[i][0]);
			continue;
		}
		st = run(cmds[i][0], cmds[i], 20);
		t_check(name, st == 0, "%s (size %ld, mod 4096 = %ld)", stdesc(st),
		    (long)sb.st_size, (long)(sb.st_size % 4096));
	}
	{
		/* SVR4 false is "exit 255" */
		static char *f[] = { "/usr/bin/false", 0 };
		st = run(f[0], f, 20);
		t_check("exec_sys_false", st != -1 && WIFEXITED(st) && WEXITSTATUS(st) != 0, "%s", stdesc(st));
	}
	{
		static char *f[] = { "/sbin/sh", "-c", "exit 7", 0 };
		st = run(f[0], f, 20);
		t_check("exec_sh_exit7", st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 7, "%s", stdesc(st));
	}
	{
		static char *f[] = { "/sbin/sh", "-c", "/usr/bin/echo a | /usr/bin/wc -c > /dev/null && /usr/bin/true", 0 };
		st = run(f[0], f, 20);
		t_check("exec_sh_pipeline", st == 0, "%s", stdesc(st));
	}
}

static void
test_exec_args_env()
{
	static char *av[] = { "xh", "args", "hello world", "", 0 };
	static char *av2[] = { "xh", "env", "T_VAR", "value=with=equals", 0 };
	static char *ev[] = { "T_VAR=value=with=equals", "PATH=/usr/bin", 0 };
	pid_t pid;
	int st, fd;

	st = run("/tests/xh_d0", av, 20);
	t_check("exec_argv", st == 0, "%s", stdesc(st));

	pid = fork();
	if (pid == 0) {
		execve("/tests/xh_d0", av2, ev);
		_exit(126);
	}
	t_waitchild(pid, &st, 20);
	t_check("exec_envp", st == 0, "%s", stdesc(st));

	putenv("T_VAR=value=with=equals");
	st = run("/tests/xh_d0", av2, 20);
	t_check("exec_environ", st == 0, "%s", stdesc(st));

	errno = 0;
	t_check("exec_ENOENT", execl("/tests/nonexistent", "x", (char *)0) == -1 && errno == ENOENT,
	    "errno %d", errno);
	fd = creat("/tmp/noexec", 0644);
	write(fd, "junk\n", 5);
	close(fd);
	errno = 0;
	t_check("exec_EACCES", execl("/tmp/noexec", "x", (char *)0) == -1 && errno == EACCES,
	    "errno %d", errno);
	chmod("/tmp/noexec", 0755);
	errno = 0;
	t_check("exec_ENOEXEC", execl("/tmp/noexec", "x", (char *)0) == -1 && errno == ENOEXEC,
	    "errno %d", errno);
	unlink("/tmp/noexec");

	/* #! script */
	fd = creat("/tmp/script", 0755);
	write(fd, "#!/sbin/sh\nexit 3\n", 18);
	close(fd);
	{
		static char *sv[] = { "/tmp/script", 0 };
		st = run(sv[0], sv, 20);
		t_check("exec_hashbang", st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 3,
		    "%s", stdesc(st));
	}
	unlink("/tmp/script");
}

/* exec a program just written to /tmp (page cache path of a fresh file) */
static void
test_exec_copy()
{
	static char *av[] = { "/tmp/xhcopy", 0 };
	char buf[4096];
	int in, out, n, st;

	in = open("/tests/xh_d2049", O_RDONLY);
	out = open("/tmp/xhcopy", O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (in < 0 || out < 0) {
		t_fail("exec_fresh_copy", "open: %s", T_ERR);
		return;
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		write(out, buf, n);
	close(in);
	close(out);
	st = run(av[0], av, 20);
	t_check("exec_fresh_copy", st == 0, "%s", stdesc(st));
	st = run(av[0], av, 20);
	t_check("exec_fresh_copy_again", st == 0, "%s", stdesc(st));
	unlink("/tmp/xhcopy");
}

int
main()
{
	t_init("proc", 120);
	test_fork_wait();
	test_ppid();
	test_many_forks();
	test_vfork();
	test_exec_sizes();
	test_exec_system();
	test_exec_args_env();
	test_exec_copy();
	return t_done();
}
