/*
 * t_env.c -- guest environments: make and lock.
 *
 * Record locks on s5 (/tmp) and ufs (the Amiga SYS: volume): another
 * process sees the holder, and SIGKILL releases it.  maketos -e makes
 * two environments with distinct roots, refuses reserved and bad names,
 * and --import copies ~/TOS without its environments and without
 * changing it.  starttos -e and startmig -e hold the lock on the
 * environment's .env while they wait for their ROM (a FIFO): a second
 * session is refused, other environments and the legacy ~/TOS lock on
 * their own, and the lock goes when the launcher is killed or exits.
 * envlock, startmac's helper, holds it across exec on the legacy
 * System Folder's stamp, which it neither makes nor changes.
 */
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
#include "t.h"

#define	H	"/tmp/envh"
#define	STARTTOS "/tos/bin/starttos"
#define	MAKETOS	"/tos/bin/maketos"
#define	STARTMIG "/tests/amiga/startmig"
#define	SYSDEV	"/dev/dsk/c0d0s0"
#define	SYS	"/amiga/sys"

/* pid holding a write lock on path, 0 if none, -1 on error */
static long
holder(path)
	char *path;
{
	struct flock fl;
	int fd = open(path, O_RDWR);

	if (fd < 0)
		return -1;
	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	if (fcntl(fd, F_GETLK, &fl) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
}

/* wait up to 10 s for pid to hold path's lock */
static int
held(path, pid)
	char *path;
	pid_t pid;
{
	int i;

	for (i = 0; i < 100; i++) {
		if (holder(path) == (long)pid)
			return 1;
		poll((struct pollfd *)0, 0, 100);
	}
	return 0;
}

/* run argv with HOME=home, output to log; its pid */
static pid_t
run(home, log, argv)
	char *home, *log, **argv;
{
	static char env[300];
	pid_t p;
	int fd;

	if ((p = fork()) == 0) {
		fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		sprintf(env, "HOME=%s", home);
		putenv(env);
		execv(argv[0], argv);
		_exit(127);
	}
	return p;
}

/* exit status of argv run to completion, -1 if it didn't exit */
static int
status(home, log, argv)
	char *home, *log, **argv;
{
	int st;
	pid_t p = run(home, log, argv);

	if (p < 0 || t_waitchild(p, &st, 30) < 0 || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static int
logged(log, s)
	char *log, *s;
{
	char b[1024];
	int fd = open(log, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	b[n > 0 ? n : 0] = 0;
	return strstr(b, s) != 0;
}

/* a record lock on fs dir: seen by another process, gone with SIGKILL */
static void
fslock(name, dir)
	char *name, *dir;
{
	char path[128], n[64];
	struct flock fl;
	int fd, st;
	pid_t p;

	sprintf(path, "%s/.locktest", dir);
	close(open(path, O_RDWR | O_CREAT, 0644));
	if ((p = fork()) == 0) {
		fd = open(path, O_RDWR);
		memset((char *)&fl, 0, sizeof fl);
		fl.l_type = F_WRLCK;
		if (fcntl(fd, F_SETLK, &fl) < 0)
			_exit(1);
		pause();
		_exit(0);
	}
	sprintf(n, "%s_held", name);
	t_check(n, held(path, p), "holder %ld, child %ld", holder(path), (long)p);
	kill(p, SIGKILL);
	t_waitchild(p, &st, 10);
	sprintf(n, "%s_killed", name);
	t_check(n, holder(path) == 0, "holder %ld after SIGKILL", holder(path));
	unlink(path);
}

static void
maketos()
{
	static char *mk[] = { MAKETOS, 0, 0, 0, 0 };
	struct stat a, b, m0, m1;
	int ok;

	system("rm -rf " H);
	mkdir(H, 0755);
	if (!t_check("maketos_legacy", status(H, "/tmp/env.log", mk) == 0 &&
	    stat(H "/TOS/AUTO", &a) == 0, "maketos failed"))
		return;
	system("echo mine > " H "/TOS/MINE.TXT");
	mk[1] = "-e";
	mk[2] = "one";
	ok = status(H, "/tmp/env.log", mk) == 0;
	mk[2] = "two";
	ok &= status(H, "/tmp/env.log", mk) == 0;
	ok &= stat(H "/TOS/one/AUTO", &a) == 0 && stat(H "/TOS/two/AUTO", &b) == 0 &&
	    a.st_ino != b.st_ino;
	t_check("maketos_two_roots", ok, "~/TOS/one and ~/TOS/two");
	system("grep -s '^id=one$' " H "/TOS/one/.env > /dev/null && "
	    "grep -s '^id=two$' " H "/TOS/two/.env > /dev/null && "
	    "grep -s '^created=' " H "/TOS/two/.env > /dev/null && echo ok > /tmp/env.ok");
	t_check("maketos_env_file", unlink("/tmp/env.ok") == 0, ".env lacks id or created");
	mk[2] = "one";
	t_check("maketos_exists", status(H, "/tmp/env.log", mk) == 1, "second maketos -e one");
	mk[2] = "Shared";
	ok = status(H, "/tmp/env.log", mk) == 1;
	mk[2] = "../x";
	ok &= status(H, "/tmp/env.log", mk) == 1;
	mk[2] = ".x";
	ok &= status(H, "/tmp/env.log", mk) == 1;
	t_check("maketos_bad_names", ok && stat(H "/TOS/Shared", &a) < 0, "Shared, ../x or .x accepted");

	stat(H "/TOS/MINE.TXT", &m0);
	mk[2] = "imp";
	mk[3] = "--import";
	ok = status(H, "/tmp/env.log", mk) == 0;
	stat(H "/TOS/MINE.TXT", &m1);
	t_check("import_copy", ok && stat(H "/TOS/imp/MINE.TXT", &a) == 0 &&
	    stat(H "/TOS/imp/AUTO", &a) == 0, "maketos -e imp --import");
	t_check("import_no_envs", stat(H "/TOS/imp/one", &a) < 0 && stat(H "/TOS/imp/imp", &a) < 0,
	    "environments copied into the import");
	t_check("import_source", m0.st_mtime == m1.st_mtime && m0.st_size == m1.st_size &&
	    m0.st_ino == m1.st_ino && stat(H "/TOS/.env", &a) < 0, "~/TOS changed");
	t_check("import_no_merge", status(H, "/tmp/env.log", mk) == 1, "second import");
	mk[3] = 0;
}

/* one launcher session: held, a second refused, released by kill or exit */
static void
sessions(name, home, av, meta, fifo)
	char *name, *home, **av, *meta, *fifo;
{
	char n[64];
	int st, fd;
	pid_t p, q;

	unlink(fifo);
	mkfifo(fifo, 0644);
	p = run(home, "/tmp/env1.log", av);
	sprintf(n, "%s_held", name);
	if (!t_check(n, held(meta, p), "holder %ld, launcher %ld", holder(meta), (long)p)) {
		system("cat /tmp/env1.log");
		kill(p, SIGKILL);
		t_waitchild(p, &st, 10);
		return;
	}
	q = run(home, "/tmp/env2.log", av);
	sprintf(n, "%s_second", name);
	t_check(n, t_waitchild(q, &st, 20) >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1 &&
	    logged("/tmp/env2.log", "in use"), "second session: status %#x", st);
	kill(p, SIGKILL);
	t_waitchild(p, &st, 10);
	sprintf(n, "%s_killed", name);
	t_check(n, holder(meta) == 0, "holder %ld after SIGKILL", holder(meta));

	/* a short ROM: the launcher exits on its own */
	p = run(home, "/tmp/env1.log", av);
	held(meta, p);
	if ((fd = open(fifo, O_WRONLY)) >= 0) {
		write(fd, "short", 5);
		close(fd);
	}
	sprintf(n, "%s_exit", name);
	t_check(n, t_waitchild(p, &st, 20) >= 0 && WIFEXITED(st) && holder(meta) == 0,
	    "status %#x, holder %ld", st, holder(meta));
	unlink(fifo);
}

static void
tos()
{
	static char *av[] = { STARTTOS, "-e", "one", "-rom", "/tmp/rom.one", 0 };
	static char *lv[] = { STARTTOS, "-rom", "/tmp/rom.leg", 0 };
	pid_t p, q, r;
	int st;

	sessions("tos", H, av, H "/TOS/one/.env", "/tmp/rom.one");
	/* two environments and the legacy tree at once */
	mkfifo("/tmp/rom.one", 0644);
	mkfifo("/tmp/rom.leg", 0644);
	p = run(H, "/tmp/env1.log", av);
	av[2] = "two";
	q = run(H, "/tmp/env2.log", av);
	r = run(H, "/tmp/env3.log", lv);
	t_check("tos_three_roots", held(H "/TOS/one/.env", p) && held(H "/TOS/two/.env", q) &&
	    held(H "/TOS/.env", r), "holders %ld %ld %ld", holder(H "/TOS/one/.env"),
	    holder(H "/TOS/two/.env"), holder(H "/TOS/.env"));
	kill(p, SIGKILL);
	kill(q, SIGKILL);
	kill(r, SIGKILL);
	t_waitchild(p, &st, 10);
	t_waitchild(q, &st, 10);
	t_waitchild(r, &st, 10);
	unlink("/tmp/rom.one");
	unlink("/tmp/rom.leg");
	av[2] = "nothere";
	t_check("tos_missing", status(H, "/tmp/env1.log", av) == 1 &&
	    logged("/tmp/env1.log", "maketos -e nothere"), "starttos -e nothere");
	av[2] = "Shared";
	t_check("tos_reserved", status(H, "/tmp/env1.log", av) == 1, "starttos -e Shared");
}

static void
amiga()
{
	static char *av[] = { STARTMIG, "-e", "u1", "-rom", "/tmp/rom.mig", 0 };
	struct stat sb;

	if (stat(SYSDEV, &sb) < 0 || stat(STARTMIG, &sb) < 0) {
		t_skip("amiga", "no SYS: volume or startmig on this root");
		return;
	}
	if (!t_check("ufs_mount", system("/sbin/mount -F ufs " SYSDEV " " SYS) == 0, "mount failed"))
		return;
	fslock("ufs_lock", SYS);
	/* an environment on ufs: HOME is a directory of the volume */
	system("mkdir -p " SYS "/.envh/Amiga/u1");
	sessions("amiga", SYS "/.envh", av, SYS "/.envh/Amiga/u1/.env", "/tmp/rom.mig");
	av[2] = "nothere";
	t_check("amiga_missing", status(SYS "/.envh", "/tmp/env1.log", av) == 1 &&
	    logged("/tmp/env1.log", "makeamiga -e nothere"), "startmig -e nothere");
	system("rm -rf " SYS "/.envh");
	t_check("ufs_umount", system("/sbin/umount " SYS) == 0, "umount failed");
}

static void
mac()
{
#define	STAMP	H "/System Folder/.stamp"
	static char *av[] = { "/tests/envlock", STAMP, "/bin/sleep", "60", 0 };
	static char *tv[] = { "/tests/envlock", "-t", STAMP, 0 };
	struct stat a, b;
	pid_t p;
	int st;

	mkdir(H, 0755);
	mkdir(H "/System Folder", 0755);
	t_check("mac_no_stamp", status(H, "/tmp/env2.log", tv) == 0 && stat(STAMP, &a) < 0,
	    "envlock -t without a stamp");
	system("echo 1 > '" STAMP "'");
	stat(STAMP, &a);
	p = run(H, "/tmp/env1.log", av);
	t_check("mac_held_across_exec", held(STAMP, p), "holder %ld, sleep %ld",
	    holder(STAMP), (long)p);
	t_check("mac_second", status(H, "/tmp/env2.log", tv) == 1 &&
	    logged("/tmp/env2.log", "in use"), "envlock -t while held");
	kill(p, SIGKILL);
	t_waitchild(p, &st, 10);
	t_check("mac_killed", status(H, "/tmp/env2.log", tv) == 0, "envlock -t after SIGKILL");
	t_check("mac_stamp_kept", stat(STAMP, &b) == 0 && b.st_size == a.st_size &&
	    b.st_mtime == a.st_mtime && stat(H "/System Folder/.env", &b) < 0,
	    "stamp changed or .env made");
}

main()
{
	struct stat sb;
	int fd;

	t_init("env", 240);
	fslock("s5_lock", "/tmp");
	if (stat(MAKETOS, &sb) < 0 || stat("/tos/sys", &sb) < 0)
		t_skip("maketos", "no maketos or /tos/sys on this root");
	else
		maketos();
	if (stat(STARTTOS, &sb) < 0 || stat(H "/TOS/two", &sb) < 0 ||
	    (fd = open("/dev/tos", O_RDWR)) < 0)
		t_skip("tos", "no starttos, environments or /dev/tos");
	else {
		close(fd);
		tos();
	}
	amiga();
	if (stat("/tests/envlock", &sb) < 0)
		t_skip("mac", "no envlock on this root");
	else
		mac();
	system("rm -rf " H);
	return t_done();
}
