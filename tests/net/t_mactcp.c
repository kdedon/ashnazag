/*
 * t_mactcp.c -- MacTCP under startmac: A/UX's .IPP driver (in the
 * patch file) over the personality's sockets, on the user network.
 *
 * Runs startmac with a System Folder whose startup item is mtcp
 * (mtcp/mtcp.c): it opens .IPP, reads the address,
 * opens a TCP stream to the echo service at 10.0.2.100:7, sends, reads
 * the echo back and closes, sends itself a datagram and resolves names
 * through "MacTCP DNR".  Its "mtcp P|F|I" lines become results.
 * Built with SYS76 (t_mactcp76.c), the same under Mac OS 7.6.1 from
 * the System Folders' volume (SCSI disk 1).
 * Skips without guest support, the uinter module, the Mac files or a
 * working echo service.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mkdev.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include "sys/mod.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	MTDIR	"/mac/sys/MTcp"
#ifdef SYS76
#define	TNAME	"mactcp76"
#define	MACVOL	"/macsys"
#define	MACDEV	"/dev/dsk/c1d0s0"
#define	SYSDIR	MACVOL "/S761"
#define	TBMEM	"TBMEMORY=16M"
#define	MACWAIT	180
#else
#define	TNAME	"mactcp"
#define	SYSDIR	MTDIR
#define	TBMEM	"TBMEMORY=8M"
#define	MACWAIT	120
#endif
#define	TLOW	0x744c4f57		/* 'tLOW', the Mac RAM segment */
#define	ECHO	0x0a000264
#define	TBUF	8192		/* the kernel's trace ring */

extern int getksym();
static char klog[TBUF + 1];

static char *macenv[] = {
	"PATH=/usr/bin:/sbin", "HOME=/tmp", "TBVERBOSE=1", "TBWARN=1",
	"TBSYSTEM=" SYSDIR, TBMEM, 0
};

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

static long
sym(name, v, wr)
char *name;
long v;
int wr;
{
	unsigned long a = 0, info;

	if (getksym(name, &a, &info) < 0 || kmemrw(a, &v, wr) < 0)
		return -1;
	return v;
}

/* the Mac task's failed calls and opens from the trace ring, as "ktrace|" lines */
static void
trace(pid)
pid_t pid;
{
	unsigned long a = 0, info;
	long pos, n, from, len = 0;
	char *s, *z;
	int fd;

	if (getksym("aux_tbuf", &a, &info) < 0 || (pos = sym("aux_tpos", 0L, 0)) < 0 ||
	    (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return;
	from = pos > TBUF ? pos % TBUF : 0;
	n = pos > TBUF ? TBUF - from : pos;
	if (lseek(fd, (off_t)(a + from), 0) != -1 && read(fd, klog, n) == n)
		len = n;
	if (pos > TBUF && lseek(fd, (off_t)a, 0) != -1 && read(fd, klog + len, from) == from)
		len += from;
	close(fd);
	klog[len] = 0;
	for (s = klog; *s; s = z) {
		for (z = s; *z && *z != '\n'; z++)
			;
		if (atoi(s) == pid)
			printf("ktrace| %.*s\n", (int)(z - s), s);
		if (*z)
			z++;
	}
	fflush(stdout);
}

/* from a file on another file system */
static int
copy(from, to)
char *from, *to;
{
	char b[4096];
	int f, t, n, ok = 0;

	if ((f = open(from, O_RDONLY)) < 0)
		return -1;
	if ((t = creat(to, 0644)) >= 0) {
		while ((n = read(f, b, sizeof b)) > 0 && write(t, b, n) == n)
			;
		ok = n == 0;
		close(t);
	}
	close(f);
	return ok ? 0 : -1;
}

/* the File ID daemon, which the Mac side's file system waits for */
static pid_t
fidd()
{
	struct stat sb;
	pid_t pid;
	int st = -1;
	FILE *f;

	/* A/UX's mount table, which fidd reads: the root and its device */
	if (stat("/etc/mtab", &sb) < 0 && stat("/", &sb) == 0 &&
	    (f = fopen("/etc/mtab", "w")) != 0) {
		fprintf(f, "/dev/dsk/c0d0s0 / 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		fclose(f);
	}
#ifdef SYS76
	/* and the System Folders' volume */
	if (stat(MACVOL, &sb) == 0 && (f = fopen("/etc/mtab", "a")) != 0) {
		fprintf(f, MACDEV " " MACVOL " 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		fclose(f);
	}
#endif
	if ((pid = fork()) == 0) {
		setpgrp();
		execl("/etc/aux/fidd", "fidd", "-d", (char *)0);
		_exit(127);
	}
	/* it forks and the parent returns */
	t_waitchild(pid, &st, 10);
	t_check("fidd", st == 0, "status %#x", st);
	return pid;
}

/* the echo service answers a native connection */
static int
echo_up()
{
	struct sockaddr_in sin;
	char b[4];
	int s, ok;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return 0;
	memset(&sin, 0, sizeof sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(7);
	sin.sin_addr.s_addr = htonl(ECHO);
	ok = connect(s, (struct sockaddr *)&sin, sizeof sin) == 0 &&
	    write(s, "ping", 4) == 4 && read(s, b, 4) > 0;
	close(s);
	return ok;
}

/* abi sock: the personality's socket calls; "P|F|S name" lines become results */
static void
abisock()
{
	static char *av[] = { "/aux/bin/abi", "sock", 0 };
	static char *env[] = { "PATH=/bin", 0 };
	char b[2048], *l, *e, *why;
	int p[2], n = 0, k, st = -1, done = 0;
	struct pollfd pf;
	long t0;
	pid_t pid;

	if (pipe(p) < 0)
		return;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		for (k = 3; k < 20; k++)
			close(k);
		execve(av[0], av, env);
		_exit(127);
	}
	close(p[1]);
	t0 = t_now_ms();
	pf.fd = p[0];
	pf.events = POLLIN;
	while (n < sizeof b - 1 && t_now_ms() - t0 < 40000L) {
		pf.revents = 0;
		if (poll(&pf, 1L, 1000) <= 0)
			continue;
		if ((k = read(p[0], b + n, sizeof b - 1 - n)) <= 0)
			break;
		n += k;
	}
	b[n] = 0;
	close(p[0]);
	kill(pid, SIGKILL);
	t_waitchild(pid, &st, 10);
	printf("abi| %s", b);
	fflush(stdout);
	for (l = b; *l; l = e + 1) {
		if ((e = strchr(l, '\n')) == 0)
			break;
		*e = 0;
		if (strcmp(l, "done") == 0)
			done = 1;
		if (e - l < 3 || l[1] != ' ')
			continue;
		if ((why = strchr(l + 2, ':')) != 0)
			*why++ = 0;
		else
			why = "";
		if (l[0] == 'P')
			t_pass(l + 2);
		else if (l[0] == 'S')
			t_skip(l + 2, "%s", why);
		else
			t_fail(l + 2, "%s", why);
	}
	t_check("sock_run", done && st != -1 && WIFEXITED(st), "status %#x", st);
}

/* startmac's output: "mtcp" lines become results, the rest is logged */
static int
relay(fd, secs)
int fd, secs;
{
	char b[512], l[600], *v;
	struct pollfd pf;
	int n, k = 0, i, done = 0;
	long t0 = t_now_ms();

	pf.fd = fd;
	pf.events = POLLIN;
	while (!done && t_now_ms() - t0 < secs * 1000L) {
		pf.revents = 0;
		if (poll(&pf, 1L, 500) <= 0)
			continue;
		if ((n = read(fd, b, sizeof b)) <= 0)
			break;
		for (i = 0; i < n; i++) {
			if (b[i] == '\n' || k == sizeof l - 8) {
				l[k] = 0;
				k = 0;
				if (strncmp(l, "mtcp ", 5) != 0) {
					if (!strstr(l, "anic"))
						printf("mac| %s\n", l);
					fflush(stdout);
					continue;
				}
				v = strchr(l + 7, ' ');
				if (v)
					*v++ = 0;
				if (strcmp(l + 5, "done") == 0)
					done = 1;
				else if (l[5] == 'P')
					t_pass(l + 7);
				else if (l[5] == 'F')
					t_fail(l + 7, "%s", v ? v : "");
				else if (l[5] == 'I')
					t_info(l + 7, "%s", v ? v : "");
			}
			else if (b[i] >= ' ' && b[i] < 0x7f)
				l[k++] = b[i];
		}
	}
	return done;
}

int
main()
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	struct mod_mreg reg;
	struct mod_execreg er;
	struct stat sb;
	int p[2], st, fd, id, mj = 54;
	pid_t pid, fpg;

	t_init(TNAME, MACWAIT + 80);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	modpath(MD);
	strcpy(reg.md_modname, "auxexec");
	reg.md_typedata = (caddr_t)&er;
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	modadm(MOD_TY_EXEC, MOD_C_MREG, &reg);
#ifndef SYS76
	if (stat("/aux/bin/abi", &sb) == 0) {
		sym("aux_trace", 2L, 1);
		abisock();
		sym("aux_trace", 0L, 1);
	}
#else
	if (stat(MACDEV, &sb) < 0) {
		t_skip("all", "no Mac OS 7.6.1 volume");
		return t_done();
	}
	if (stat(SYSDIR, &sb) < 0 &&
	    !t_check("macvol_mount", system("/sbin/mount -F ufs " MACDEV " " MACVOL) == 0,
	    "mount " MACDEV " failed"))
		return t_done();
#endif
	if (stat(MD "/uinter", &sb) < 0 || stat("/mac/bin/startmac", &sb) < 0 ||
	    stat("/etc/aux/rom", &sb) < 0 || stat(MTDIR "/mtcp", &sb) < 0 || stat(SYSDIR, &sb) < 0) {
		t_skip("all", "no uinter module, startmac, ROM, fidd or mtcp on this root");
		return t_done();
	}
	if (!echo_up()) {
		t_skip("all", "no echo service at 10.0.2.100:7");
		return t_done();
	}
	strcpy(reg.md_modname, "uinter");
	reg.md_typedata = (caddr_t)&mj;
	modadm(MOD_TY_CDEV, MOD_C_MREG, &reg);
	/* registered by an earlier test is fine: the open loads it */
	if (!t_check("uinter_open", (fd = open("/dev/uinter0", O_RDWR)) >= 0, "%s", T_ERR))
		return t_done();
	close(fd);
	fpg = fidd();
#ifdef SYS76
	copy(MTDIR "/mtcp", SYSDIR "/Startup Items/mtcp");
	copy(MTDIR "/MacTCPDNR", SYSDIR "/MacTCP DNR");
#else
	link("/mac/sys/Sys7/System", SYSDIR "/System");
	link("/mac/sys/Sys7/Finder", SYSDIR "/Finder");
	mkdir(SYSDIR "/Startup Items", 0777);
	link(SYSDIR "/mtcp", SYSDIR "/Startup Items/mtcp");
	link(SYSDIR "/MacTCPDNR", SYSDIR "/MacTCP DNR");
#endif
	mkdir("/Desktop Folder", 0777);
	if (pipe(p) < 0)
		return t_done();
	sym("aux_tpos", 0L, 1);
	sym("aux_trace", 4L | 8L, 1);
	if ((pid = fork()) == 0) {
		setpgrp();
		dup2(p[1], 1);
		dup2(p[1], 2);
		for (fd = 3; fd < 20; fd++)
			close(fd);
		execve(av[0], av, macenv);
		_exit(127);
	}
	close(p[1]);
	t_check("mtcp_done", relay(p[0], MACWAIT), "no \"mtcp done\" from the Mac side");
	kill(-pid, SIGKILL);
	close(p[0]);
	st = 0;
	t_waitchild(pid, &st, 20);
	sym("aux_trace", 0L, 1);
	trace(pid);
	if ((id = shmget(TLOW, 0, 0)) >= 0)
		shmctl(id, IPC_RMID, (struct shmid_ds *)0);
	kill(-fpg, SIGTERM);
#ifndef SYS76
	unlink(SYSDIR "/System");
	unlink(SYSDIR "/Finder");
#endif
	unlink(SYSDIR "/Startup Items/mtcp");
	unlink(SYSDIR "/MacTCP DNR");
	return t_done();
}
