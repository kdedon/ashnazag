/*
 * mintrun -- run a MiNT program (a TOS PRG built with MiNTLib) as a Unix
 * process.
 *
 *   mintrun prog [args]
 *
 * The program's memory is mapped at 0 and the process becomes a lone TOS
 * guest (TEF_NOMACH): no ROM, no machine.  Its GEMDOS/MiNT calls (trap #1)
 * and the few BIOS/XBIOS calls a command-line program makes come here and
 * become host calls: files, fds 0-2, fork/exec/wait, signals and sockets
 * are the host's.  Paths: u:\host\x is /x, u:\bin, \usr, \etc ... are
 * under /tos/mint, \home, \tmp, \dev are the host's.
 * MINTMEM sets the memory size (default 8 MB); MINTTRACE reports calls
 * it does not serve, MINTTRACE=2 every GEMDOS call.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <termio.h>
#include "tosio.h"

#define	BP	0x10000L		/* the basepage */
#define	JAR	0x700L			/* the cookie jar */
#define	NFD	256
#define	NDIR	32

#define	W(o)	(*(short *)(a + (o)))
#define	UW(o)	(*(unsigned short *)(a + (o)))
#define	L(o)	(*(long *)(a + (o)))
#define	P(o)	((char *)L(o))
#define	PUT16(p, v)	(*(short *)(p) = (v))
#define	PUT32(p, v)	(*(long *)(p) = (long)(v))

/* MiNT errors */
#define	M_EINVFN	32
#define	M_ENOENT	33
#define	M_EBADF		37
#define	M_ENOMEM	39
#define	M_EINVAL	25
#define	M_ENMFILES	49
#define	M_ERANGE	88
#define	M_EAFNOSUPPORT	309

extern void mint_t1(), mint_t2(), mint_t13(), mint_t14(), mint_go();
extern char **environ;

char mint_stk[0x10000];		/* the trap entries' stack */
static int tfd = -1;
static char *self = "/tos/bin/mintrun";
static unsigned long mem = 0x800000, brk0, lastblk;
static long dta;
static unsigned long envlo;
long mint_rw();
static int domain, trace;
static char sock[NFD];
static DIR *dirs[NDIR];
static int dirtos[NDIR];
static struct { long h, mask; short flags; } msa[32];
static sigset_t prevmask;

static void
die(s)
	char *s;
{
	fprintf(stderr, "mintrun: %s: %s\n", s, strerror(errno));
	exit(127);
}

/* ---- numbers ---- */

static short errtab[][2] = {
	{ EPERM, 38 }, { ENOENT, 33 }, { ESRCH, 20 }, { EINTR, 128 }, { EIO, 90 },
	{ ENXIO, 46 }, { ENOEXEC, 66 }, { EBADF, 37 }, { ECHILD, 21 }, { EAGAIN, 326 },
	{ ENOMEM, 39 }, { EACCES, 36 }, { EFAULT, 40 }, { EBUSY, 2 }, { EEXIST, 85 },
	{ EXDEV, 48 }, { ENODEV, 15 }, { ENOTDIR, 34 }, { EISDIR, 24 }, { EINVAL, 25 },
	{ ENFILE, 50 }, { EMFILE, 35 }, { ENOTTY, 87 }, { ENOSPC, 91 }, { ESPIPE, 6 },
	{ EROFS, 13 }, { EMLINK, 82 }, { EPIPE, 81 }, { ERANGE, 88 }, { ENOSYS, 32 },
	{ ENAMETOOLONG, 86 }, { ENOTEMPTY, 83 }, { ELOOP, 80 }, { ENOTSOCK, 300 },
	{ EDESTADDRREQ, 301 }, { EMSGSIZE, 302 }, { EPROTOTYPE, 303 }, { ENOPROTOOPT, 304 },
	{ EPROTONOSUPPORT, 305 }, { ESOCKTNOSUPPORT, 306 }, { EOPNOTSUPP, 307 },
	{ EAFNOSUPPORT, 309 }, { EADDRINUSE, 310 }, { EADDRNOTAVAIL, 311 },
	{ ENETDOWN, 312 }, { ENETUNREACH, 313 }, { ENETRESET, 314 }, { ECONNABORTED, 315 },
	{ ECONNRESET, 316 }, { EISCONN, 317 }, { ENOTCONN, 318 }, { ESHUTDOWN, 319 },
	{ ETIMEDOUT, 320 }, { ECONNREFUSED, 321 }, { EHOSTDOWN, 322 }, { EHOSTUNREACH, 323 },
	{ EALREADY, 324 }, { EINPROGRESS, 325 }, { ENOBUFS, 327 }, { EDEADLK, 22 },
	{ ETXTBSY, 70 }, { EFBIG, 71 }, { ENOLCK, 110 }, { 0, 0 }
};

/* a host errno as a MiNT error: negative */
static long
merr(e)
	int e;
{
	int i;

	for (i = 0; errtab[i][0]; i++)
		if (errtab[i][0] == e)
			return -errtab[i][1];
	return -M_EINVAL;
}

/* a host call's result: its value, or the MiNT error */
static long
RV(x)
	long x;
{
	return x < 0 ? merr(errno) : x;
}

/* MiNT signal numbers by host number */
static short sigtab[][2] = {
	{ SIGHUP, 1 }, { SIGINT, 2 }, { SIGQUIT, 3 }, { SIGILL, 4 }, { SIGTRAP, 5 },
	{ SIGABRT, 6 }, { SIGEMT, 7 }, { SIGFPE, 8 }, { SIGKILL, 9 }, { SIGBUS, 10 },
	{ SIGSEGV, 11 }, { SIGSYS, 12 }, { SIGPIPE, 13 }, { SIGALRM, 14 }, { SIGTERM, 15 },
	{ SIGURG, 16 }, { SIGSTOP, 17 }, { SIGTSTP, 18 }, { SIGCONT, 19 }, { SIGCHLD, 20 },
	{ SIGTTIN, 21 }, { SIGTTOU, 22 }, { SIGPOLL, 23 }, { SIGXCPU, 24 }, { SIGXFSZ, 25 },
	{ SIGVTALRM, 26 }, { SIGPROF, 27 }, { SIGWINCH, 28 }, { SIGUSR1, 29 },
	{ SIGUSR2, 30 }, { SIGPWR, 31 }, { 0, 0 }
};

static int
msig(h)
	int h;
{
	int i;

	for (i = 0; sigtab[i][0]; i++)
		if (sigtab[i][0] == h)
			return sigtab[i][1];
	return 0;
}

static int
hsig(m)
	int m;
{
	int i;

	for (i = 0; sigtab[i][0]; i++)
		if (sigtab[i][1] == m)
			return sigtab[i][0];
	return 0;
}

static unsigned long
mmask(s)
	sigset_t *s;
{
	unsigned long m = 0;
	int i;

	for (i = 0; sigtab[i][0]; i++)
		if (sigismember(s, sigtab[i][0]))
			m |= 1L << sigtab[i][1];
	return m;
}

static void
hmask(m, s)
	unsigned long m;
	sigset_t *s;
{
	int i;

	sigemptyset(s);
	for (i = 0; sigtab[i][0]; i++)
		if (m & (1L << sigtab[i][1]))
			sigaddset(s, sigtab[i][0]);
}

/* a host wait status in MiNT's 16 bits: exit code low, signal << 8 */
static long
mstat(st)
	int st;
{
	if (WIFSIGNALED(st))
		return msig(WTERMSIG(st)) << 8;
	if (WIFSTOPPED(st))
		return msig(WSTOPSIG(st)) << 8 | 0x7f;
	return WEXITSTATUS(st);
}

/* MiNT open flags to the host's */
static int
oflags(m)
	long m;
{
	int f = m & 3;

	if (m & 0x100) f |= O_NONBLOCK;
	if (m & 0x200) f |= O_CREAT;
	if (m & 0x400) f |= O_TRUNC;
	if (m & 0x800) f |= O_EXCL;
	if (m & 0x1000) f |= O_APPEND;
	if (m & 0x4000) f |= O_NOCTTY;
	return f;
}

static long
mflags(f)
	int f;
{
	long m = f & 3;

	if (f & O_NONBLOCK) m |= 0x100;
	if (f & O_APPEND) m |= 0x1000;
	return m;
}

/* ---- paths ---- */

static char *mintdirs[] = { "bin", "sbin", "usr", "etc", "opt", "var", 0 };

/* a MiNT path in host terms */
static char *
hpath(p, buf)
	char *p, *buf;
{
	char t[1024], *s, *h;
	int i, n;

	h = p[0] && p[1] == ':' && (p[0] | 0x20) == 'c' ? getenv("HOME") : "";
	if (strlen(p) + strlen(h ? h : "") + 16 > sizeof t) {
		buf[0] = 0;		/* fails with ENOENT */
		return buf;
	}
	if (p[0] && p[1] == ':') {
		if ((p[0] | 0x20) == 'c') {
			h = getenv("HOME");
			sprintf(t, "%s/TOS/%s", h ? h : "", p + 2);
		} else
			strcpy(t, p + 2);
	} else
		strncpy(t, p, sizeof t - 1), t[sizeof t - 1] = 0;
	for (s = t; *s; s++)
		if (*s == '\\')
			*s = '/';
	if (t[0] != '/') {
		strcpy(buf, t);
		return buf;
	}
	if (strncmp(t, "/host", 5) == 0 && (t[5] == '/' || t[5] == 0)) {
		strcpy(buf, t[5] ? t + 5 : "/");
		return buf;
	}
	for (i = 0; mintdirs[i]; i++) {
		n = strlen(mintdirs[i]);
		if (strncmp(t + 1, mintdirs[i], n) == 0 && (t[n + 1] == '/' || t[n + 1] == 0)) {
			sprintf(buf, "/tos/mint%s", t);
			return buf;
		}
	}
	strcpy(buf, t);
	return buf;
}

/* a host path as MiNT sees it, with backslashes, no drive */
static void
mpath(h, buf)
	char *h, *buf;
{
	char *s;

	if (strncmp(h, "/tos/mint/", 10) == 0)
		strcpy(buf, h + 9);
	else if (strncmp(h, "/home", 5) == 0 || strncmp(h, "/tmp", 4) == 0 ||
	    strncmp(h, "/dev", 4) == 0)
		strcpy(buf, h);
	else
		sprintf(buf, "/host%s", strcmp(h, "/") ? h : "");
	for (s = buf; *s; s++)
		if (*s == '/')
			*s = '\\';
}

/* ---- files ---- */

static long
dosdt(t)
	time_t t;
{
	struct tm *tm = localtime(&t);

	return (long)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday) << 16 |
	    (tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
}

static long
mmode(m)
	long m;
{
	long t;

	switch (m & S_IFMT) {
	case S_IFDIR: t = 0040000; break;
	case S_IFCHR: t = 0020000; break;
	case S_IFBLK: t = 0060000; break;
	case S_IFIFO: t = 0120000; break;
	case S_IFLNK: t = 0160000; break;
	default: t = 0100000;
	}
	return t | (m & 07777);
}

/* MiNT's 128-byte struct stat */
static void
st64(sb, o)
	struct stat *sb;
	char *o;
{
	memset(o, 0, 128);
	PUT32(o + 4, sb->st_dev);
	PUT32(o + 8, sb->st_ino);
	PUT32(o + 12, mmode((long)sb->st_mode));
	PUT32(o + 16, sb->st_nlink);
	PUT32(o + 20, sb->st_uid);
	PUT32(o + 24, sb->st_gid);
	PUT32(o + 32, sb->st_rdev);
	PUT32(o + 40, sb->st_atime);
	PUT32(o + 52, sb->st_mtime);
	PUT32(o + 64, sb->st_ctime);
	PUT32(o + 76, sb->st_size);
	PUT32(o + 84, sb->st_blocks);
	PUT32(o + 88, sb->st_blksize);
}

/* the older struct xattr: DOS times */
static void
xattr(sb, o)
	struct stat *sb;
	char *o;
{
	long t;

	memset(o, 0, 52);
	PUT16(o, mmode((long)sb->st_mode));
	PUT32(o + 2, sb->st_ino);
	PUT16(o + 6, sb->st_dev);
	PUT16(o + 8, sb->st_rdev);
	PUT16(o + 10, sb->st_nlink);
	PUT16(o + 12, sb->st_uid);
	PUT16(o + 14, sb->st_gid);
	PUT32(o + 16, sb->st_size);
	PUT32(o + 20, 1024);
	PUT32(o + 24, (sb->st_size + 1023) / 1024);
	t = dosdt(sb->st_mtime);
	PUT16(o + 28, t), PUT16(o + 30, t >> 16);
	t = dosdt(sb->st_atime);
	PUT16(o + 32, t), PUT16(o + 34, t >> 16);
	t = dosdt(sb->st_ctime);
	PUT16(o + 36, t), PUT16(o + 38, t >> 16);
	PUT16(o + 40, S_ISDIR(sb->st_mode) ? 0x10 : (sb->st_mode & 0222) ? 0 : 1);
}

static long
dopen(p, flags)
	char *p;
	int flags;
{
	char b[1024];
	int i;

	for (i = 0; i < NDIR && dirs[i]; i++)
		;
	if (i == NDIR)
		return -35;
	if ((dirs[i] = opendir(hpath(p, b))) == 0)
		return merr(errno);
	dirtos[i] = flags & 1;
	return 0x7d000000L | i;
}

static DIR *
dget(h)
	long h;
{
	int i = h & 0xffff;

	return (h & 0xffff0000L) == 0x7d000000L && i < NDIR ? dirs[i] : 0;
}

static long
dread(len, h, buf)
	int len;
	long h;
	char *buf;
{
	DIR *d = dget(h);
	struct dirent *e;
	int n, tos;

	if (!d)
		return -M_EBADF;
	tos = dirtos[h & 0xffff];
	if ((e = readdir(d)) == 0)
		return -M_ENMFILES;
	n = strlen(e->d_name) + 1;
	if (len < n + (tos ? 0 : 4))
		return -M_ERANGE;
	if (!tos) {
		PUT32(buf, e->d_ino);
		buf += 4;
	}
	memcpy(buf, e->d_name, n);
	return 0;
}

/* ---- memory: one bump region above the program ---- */

static long
mxalloc(n)
	long n;
{
	unsigned long a;

	if (n == -1)
		return envlo - brk0 - 0x1000;
	a = (brk0 + 15) & ~15;
	if (n < 0 || a + n + 0x1000 > envlo)
		return 0;
	brk0 = a + n;
	lastblk = a;
	memset((char *)a, 0, n);
	return a;
}

/* ---- sockets over /dev/tos ---- */

static long
sk(op, fd, arg, buf, len, addr, alenp)
	int op, fd;
	long arg, len, *alenp;
	char *buf, *addr;
{
	struct tossock so;
	char gap[576];

	memset((char *)&so, 0, sizeof so);
	so.so_op = op;
	so.so_fd = fd;
	so.so_arg = arg;
	so.so_buf = buf;
	so.so_len = len;
	so.so_gap = gap;
	if (addr && alenp && *alenp > 0 && op != TSO_ACCEPT && op != TSO_NAME &&
	    op != TSO_RECV) {
		so.so_alen = *alenp > 16 ? 16 : *alenp;
		memcpy(so.so_addr, addr, (int)so.so_alen);
	}
	if (ioctl(tfd, TOSIOC_SOCK, &so) < 0)
		return merr(errno);
	if (addr && alenp && (op == TSO_ACCEPT || op == TSO_NAME || op == TSO_RECV)) {
		memcpy(addr, so.so_addr, *alenp < 0 ? 0 : *alenp < 16 ? (int)*alenp : 16);
		*alenp = 16;
	}
	return so.so_rv;
}

/* MiNT's SOL_SOCKET option names, by number */
static short soname[] = {
	0, SO_DEBUG, SO_REUSEADDR, SO_TYPE, SO_ERROR, SO_DONTROUTE, SO_BROADCAST,
	SO_SNDBUF, SO_RCVBUF, SO_KEEPALIVE, SO_OOBINLINE, SO_LINGER, 0, 0, 0, 0, 0, 0,
	SO_RCVLOWAT, SO_SNDLOWAT, SO_RCVTIMEO, SO_SNDTIMEO
};

/* a MiNT socket option as the host's level << 16 | name; -304 unknown */
static long
sopt(level, name)
	long level, name;
{
	if (level != 0xffff)
		return level << 16 | (name & 0xffff);
	if (name <= 0 || name >= sizeof soname / sizeof soname[0] || !soname[name])
		return -304;			/* ENOPROTOOPT */
	return 0xffffL << 16 | soname[name];
}

static long
sockcall(op, a)
	int op;
	char *a;
{
	long r, n, *lp;
	int fd = W(2);

	if (op != 0x160 && (fd < 0 || fd >= NFD || !sock[fd]))
		return -300;			/* ENOTSOCK */
	switch (op) {
	case 0x160:				/* Fsocket */
		if (L(2) != 2)
			return -M_EAFNOSUPPORT;
		n = L(6) == 1 ? SOCK_STREAM : L(6) == 2 ? SOCK_DGRAM : L(6);
		if ((r = sk(TSO_SOCKET, 0, n, (char *)0, 0L, (char *)0, (long *)0)) >= 0 && r < NFD)
			sock[r] = 1;
		return r;
	case 0x162:				/* Faccept */
		n = 16;
		lp = (long *)L(8);
		r = sk(TSO_ACCEPT, fd, 0L, (char *)0, 0L, P(4) ? P(4) : (char *)0,
		    P(4) && lp ? lp : (long *)0);
		if (r >= 0 && r < NFD)
			sock[r] = 1;
		return r;
	case 0x163:				/* Fconnect */
	case 0x164:				/* Fbind */
		n = L(8);
		r = sk(op == 0x163 ? TSO_CONNECT : TSO_BIND, fd, 0L, (char *)0, 0L, P(4), &n);
		return r < 0 ? r : 0;
	case 0x165:				/* Flisten */
		return (r = sk(TSO_LISTEN, fd, L(4), (char *)0, 0L, (char *)0, (long *)0)) < 0 ? r : 0;
	case 0x168:				/* Frecvfrom */
		lp = (long *)L(20);
		return sk(TSO_RECV, fd, L(12), P(4), L(8), lp ? P(16) : (char *)0, lp);
	case 0x169:				/* Fsendto */
		n = L(20);
		return sk(TSO_SEND, fd, L(12), P(4), L(8), P(16), P(16) ? &n : (long *)0);
	case 0x16a:				/* Fsetsockopt */
		if ((n = sopt(L(4), L(8))) == -304)
			return n;
		return (r = sk(TSO_SETOPT, fd, n, P(12), L(16), (char *)0, (long *)0)) < 0 ? r : 0;
	case 0x16b:				/* Fgetsockopt */
		if ((lp = (long *)L(16)) == 0)
			return -M_EINVAL;
		if ((n = sopt(L(4), L(8))) == -304)
			return n;
		if ((r = sk(TSO_GETOPT, fd, n, P(12), *lp, (char *)0, (long *)0)) < 0)
			return r;
		*lp = r;
		if (n == (0xffffL << 16 | SO_TYPE))
			PUT32(P(12), *(long *)P(12) == SOCK_STREAM ? 1 : 2);
		else if (n == (0xffffL << 16 | SO_ERROR) && *(long *)P(12))
			PUT32(P(12), -merr((int)*(long *)P(12)));
		return 0;
	case 0x16c:				/* Fgetpeername */
	case 0x16d:				/* Fgetsockname */
		return (r = sk(TSO_NAME, fd, op == 0x16c ? 1L : 0L, (char *)0, 0L, P(4),
		    (long *)L(8))) < 0 ? r : 0;
	case 0x16e:				/* Fshutdown */
		return (r = sk(TSO_SHUTDOWN, fd, L(4), (char *)0, 0L, (char *)0, (long *)0)) < 0 ? r : 0;
	}
	return -M_EINVFN;
}

/* a socket descriptor of the host's */
static int
issock(fd)
	int fd;
{
	char a[16];
	long n = 16;

	return sk(TSO_NAME, fd, 0L, (char *)0, 0L, a, &n) >= 0;
}

/* the result of a dup of fd: a socket stays one */
static long
dupfd(fd, r)
	int fd;
	long r;
{
	if (r >= 0 && r < NFD)
		sock[r] = fd >= 0 && fd < NFD && sock[fd];
	return r;
}

/* ---- signals ---- */

static void
catch(s)
	int s;
{
	int m = msig(s);
	sigset_t o;

	sigprocmask(SIG_BLOCK, (sigset_t *)0, &o);
	sigdelset(&o, s);
	prevmask = o;
	if (m && msa[m].h > 1)
		(*(void (*)())msa[m].h)((long)m);
}

static long
sigact(m, h, mask, flags)
	int m;
	long h, mask;
	int flags;
{
	struct sigaction sa;
	int s = hsig(m);

	if (!s || s == SIGKILL || s == SIGSTOP)
		return -M_EINVAL;
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = h == 0 ? SIG_DFL : h == 1 ? SIG_IGN : catch;
	hmask((unsigned long)mask, &sa.sa_mask);
	if (m == 20 && (flags & 1))
		sa.sa_flags |= SA_NOCLDSTOP;
	if (sigaction(s, &sa, (struct sigaction *)0) < 0)
		return merr(errno);
	msa[m].h = h;
	msa[m].mask = mask;
	msa[m].flags = flags;
	return 0;
}

/* ---- processes ---- */

/* argv and envp of a Pexec: the env block, its ARGV= tail, or the command line */
static void
pargs(path, cmd, env, av, ev, buf)
	char *path, *cmd, *env, **av, **ev, *buf;
{
	char *s, *argv0 = 0;
	int na = 0, ne = 0, n;

	av[na++] = self;
	av[na++] = path;
	for (s = env; s && *s; s += strlen(s) + 1) {
		if (strncmp(s, "ARGV=", 5) == 0) {
			s += strlen(s) + 1;
			if (*s)
				argv0 = s, s += strlen(s) + 1;
			for (; *s && na < 255; s += strlen(s) + 1)
				av[na++] = s;
			break;
		}
		if (ne < 255)
			ev[ne++] = s;
	}
	if (!argv0) {
		n = (unsigned char)cmd[0];
		if (n > 125)
			n = 125;
		memcpy(buf, cmd + 1, n);
		buf[n] = 0;
		for (s = strtok(buf, " "); s && na < 255; s = strtok((char *)0, " "))
			av[na++] = s;
	}
	av[na] = 0;
	if (!env)
		for (ne = 0; environ[ne] && ne < 255; ne++)
			ev[ne] = environ[ne];
	ev[ne] = 0;
}

static long
pexec(mode, name, cmd, env)
	int mode;
	char *name, *cmd, *env;
{
	char h[1024], b[128], m[4], *av[256], *ev[256];
	int fd, pid, st, elf;

	if (mode != 0 && mode != 100 && mode != 200)
		return -M_EINVFN;
	hpath(name, h);
	if ((fd = open(h, O_RDONLY)) < 0)
		return merr(errno);
	elf = read(fd, m, 4) == 4 && memcmp(m, "\177ELF", 4) == 0;
	close(fd);
	pargs(h, cmd, env, av, ev, b);
	if (mode != 200 && (pid = fork()) != 0) {
		if (pid < 0)
			return merr(errno);
		if (mode == 100)
			return pid;
		while (waitpid(pid, &st, 0) < 0)
			if (errno != EINTR)
				return merr(errno);
		return WIFEXITED(st) ? WEXITSTATUS(st) : -msig(WTERMSIG(st));
	}
	execve(elf ? h : self, elf ? av + 1 : av, ev);
	if (mode == 200)
		return merr(errno);
	_exit(127);
	return 0;
}

static long
pwait(pid, flags, ru)
	int pid, flags;
	long *ru;
{
	int st, r;

	do
		r = waitpid(pid, &st, ((flags & 1) ? WNOHANG : 0) | ((flags & 2) ? WUNTRACED : 0));
	while (r < 0 && errno == EINTR);
	if (r < 0)
		return merr(errno);
	if (ru)
		ru[0] = ru[1] = 0;
	if (r == 0)
		return 0;
	return (long)r << 16 | mstat(st);
}

/* ---- select and poll ---- */

static long
fselect(tmo, rp, wp, xp)
	unsigned tmo;
	long *rp, *wp, *xp;
{
	struct pollfd p[32];
	int i, n = 0, r;
	long rr = 0, wr = 0, xr = 0;

	for (i = 0; i < 32; i++) {
		p[n].fd = i;
		p[n].events = ((rp && (*rp >> i & 1)) ? POLLIN : 0) |
		    ((wp && (*wp >> i & 1)) ? POLLOUT : 0) | ((xp && (*xp >> i & 1)) ? POLLPRI : 0);
		p[n].revents = 0;
		if (p[n].events)
			n++;
	}
	if ((r = poll(p, n, tmo ? (int)tmo : -1)) < 0)
		return merr(errno);
	for (i = 0; i < n; i++) {
		if (p[i].revents & (POLLIN | POLLHUP | POLLERR) && (p[i].events & POLLIN))
			rr |= 1L << p[i].fd;
		if (p[i].revents & (POLLOUT | POLLERR) && (p[i].events & POLLOUT))
			wr |= 1L << p[i].fd;
		if (p[i].revents & POLLPRI)
			xr |= 1L << p[i].fd;
	}
	if (rp) *rp = rr;
	if (wp) *wp = wr;
	if (xp) *xp = xr;
	return r;
}

static long
fpoll(m, n, tmo)
	char *m;
	unsigned long n, tmo;
{
	struct pollfd p[64];
	int i, r;

	if (n > 64)
		return -M_EINVAL;
	for (i = 0; i < n; i++) {
		p[i].fd = *(long *)(m + 8 * i);
		p[i].events = *(short *)(m + 8 * i + 4) & 0x3f;
		p[i].revents = 0;
	}
	if ((r = poll(p, n, tmo == ~0UL ? -1 : (int)tmo)) < 0)
		return merr(errno);
	for (i = 0; i < n; i++)
		*(short *)(m + 8 * i + 6) = p[i].revents & 0x3f;
	return r;
}

/* ---- the trap #1 table ---- */

static long
fcntl1(fd, arg, cmd)
	int fd, cmd;
	long arg;
{
	struct stat sb;
	struct winsize ws;
	struct termio t;
	int v;

	switch (cmd) {
	case 0: return dupfd(fd, RV(fcntl(fd, F_DUPFD, (int)arg)));
	case 1: return RV(fcntl(fd, F_GETFD, 0));
	case 2: return RV(fcntl(fd, F_SETFD, (int)arg & 1));
	case 3: return (v = fcntl(fd, F_GETFL, 0)) < 0 ? merr(errno) : mflags(v);
	case 4: return RV(fcntl(fd, F_SETFL, oflags(arg) & ~3));
	case 0x4600:					/* FSTAT */
		if (fstat(fd, &sb) < 0)
			return merr(errno);
		xattr(&sb, (char *)arg);
		return 0;
	case 0x4606:					/* FSTAT64 */
		if (fstat(fd, &sb) < 0)
			return merr(errno);
		st64(&sb, (char *)arg);
		return 0;
	case 0x4601:					/* FIONREAD */
		if (ioctl(fd, FIONREAD, &v) < 0)
			return merr(errno);
		*(long *)arg = v;
		return 0;
	case 0x5400:					/* TIOCGETP */
		if (ioctl(fd, TCGETA, &t) < 0)
			return merr(errno);
		((char *)arg)[0] = ((char *)arg)[1] = t.c_cflag & CBAUD;
		((char *)arg)[2] = t.c_cc[VERASE];
		((char *)arg)[3] = t.c_cc[VKILL];
		PUT16((char *)arg + 4, ((t.c_lflag & ICANON) ? 0 : 0x0001) |
		    ((t.c_lflag & ECHO) ? 0x0004 : 0) | ((t.c_oflag & ONLCR) ? 0x0008 : 0));
		return 0;
	case 0x5406:					/* TIOCGPGRP */
		if ((v = tcgetpgrp(fd)) < 0)
			return merr(errno);
		*(long *)arg = v;
		return 0;
	case 0x540b:					/* TIOCGWINSZ */
		if (ioctl(fd, TIOCGWINSZ, &ws) < 0)
			return merr(errno);
		memcpy((char *)arg, (char *)&ws, 8);
		return 0;
	}
	return isatty(fd) || fstat(fd, &sb) == 0 ? -M_EINVFN : -M_EBADF;
}

long
mint_gemdos(a)
	char *a;
{
	char h[1024], h2[1040];
	struct stat sb;
	struct statvfs vf;
	struct timeval tv;
	struct itimerval it, oit;
	sigset_t ss, os;
	char c;
	int op = UW(0), fd, r;
	long *lp, v;
	time_t now;
	struct tm *tm;

	if (trace > 1)
		fprintf(stderr, "[%d $%x %lx %lx]\n", (int)getpid(), op, L(2), L(6));
	switch (op) {
	case 0x00: _exit(0);
	case 0x4c: _exit(W(2) & 0xff);
	case 0x31: _exit(W(6) & 0xff);
	case 0x01: case 0x07: case 0x08:
		return read(0, &c, 1) == 1 ? (unsigned char)c : 0;
	case 0x02: case 0x04: case 0x05:
		c = W(2);
		write(op == 0x02 ? 1 : 2, &c, 1);
		return 0;
	case 0x06:
		if ((W(2) & 0xff) == 0xff)
			return 0;
		c = W(2);
		write(1, &c, 1);
		return 0;
	case 0x09:
		return write(1, P(2), strlen(P(2)));
	case 0x0a:
		r = read(0, P(2) + 2, (unsigned char)P(2)[0]);
		if (r > 0 && P(2)[r + 1] == '\n')
			r--;
		P(2)[1] = r < 0 ? 0 : r;
		return 0;
	case 0x0b: case 0x10: case 0x11: case 0x12: case 0x13:
		return -1;
	case 0x0e: return 1L << 2 | 1L << 20;
	case 0x19: return 20;
	case 0x1a: dta = L(2); return 0;
	case 0x2f: return dta;
	case 0x20: return L(2) == 1 ? -1 : 0;
	case 0x30: return 0x2000;
	case 0x2a: case 0x2c:
		time(&now);
		v = dosdt(now);
		return op == 0x2a ? (v >> 16) & 0xffff : v & 0xffff;
	case 0x2b: case 0x2d: return -36;
	case 0x36:
		if (statvfs(".", &vf) < 0)
			return merr(errno);
		lp = (long *)L(2);
		lp[0] = vf.f_bavail; lp[1] = vf.f_blocks; lp[2] = vf.f_frsize; lp[3] = 1;
		return 0;
	case 0x39: return RV(mkdir(hpath(P(2), h), 0777));
	case 0x3a: return RV(rmdir(hpath(P(2), h)));
	case 0x3b: return RV(chdir(hpath(P(2), h)));
	case 0x3c:
		return RV(open(hpath(P(2), h), O_RDWR | O_CREAT | O_TRUNC, (W(6) & 1) ? 0444 : 0666));
	case 0x3d: return RV(open(hpath(P(2), h), oflags((long)UW(6)), 0666));
	case 0x3e:
		if ((fd = W(2)) < 0)
			return 0;
		if (fd < NFD)
			sock[fd] = 0;
		return RV(close(fd));
	case 0x3f: case 0x40:
		fd = W(2);
		if (fd >= 0 && fd < NFD && sock[fd])
			return sk(op == 0x3f ? TSO_RECV : TSO_SEND, fd, 0L, P(8), L(4),
			    (char *)0, (long *)0);
		r = op == 0x3f ? read(fd, P(8), L(4)) : write(fd, P(8), L(4));
		return RV(r);
	case 0x41:
		if (stat(hpath(P(2), h), &sb) == 0 && S_ISDIR(sb.st_mode))
			return -36;
		return RV(unlink(h));
	case 0x42: return RV(lseek(W(6), L(2), W(8)));
	case 0x43:
		if (stat(hpath(P(2), h), &sb) < 0)
			return merr(errno);
		if (W(6) && chmod(h, (W(8) & 1) ? sb.st_mode & ~0222 : sb.st_mode | 0200) < 0)
			return merr(errno);
		return S_ISDIR(sb.st_mode) ? 0x10 : (sb.st_mode & 0222) ? 0 : 1;
	case 0x44: case 0x48: return mxalloc(L(2));
	case 0x45: return dupfd(W(2), RV(dup(W(2))));
	case 0x46: return dupfd(W(4), RV(dup2(W(4), W(2))));
	case 0x47: case 0x13b:
		if (!getcwd(h, sizeof h))
			return merr(errno);
		mpath(h, h2);
		if (strlen(h2) >= (op == 0x13b ? (unsigned)W(8) : 128))
			return -M_ERANGE;
		strcpy(P(2), h2);
		return 0;
	case 0x49: if (L(2) == lastblk && lastblk) brk0 = lastblk, lastblk = 0; return 0;
	case 0x4a:
		if (L(4) == BP)
			brk0 = BP + L(8);
		else if (L(4) == lastblk)
			brk0 = lastblk + L(8);
		return 0;
	case 0x4b: return pexec(W(2), P(4), P(8), P(12));
	case 0x4e:
		if (!dta || stat(hpath(P(2), h), &sb) < 0)
			return -M_ENOENT;
		v = dosdt(sb.st_mtime);
		*(char *)(dta + 21) = S_ISDIR(sb.st_mode) ? 0x10 : 0;
		PUT16((char *)dta + 22, v), PUT16((char *)dta + 24, v >> 16);
		PUT32((char *)dta + 26, sb.st_size);
		strncpy((char *)dta + 30, strrchr(h, '/') ? strrchr(h, '/') + 1 : h, 13);
		((char *)dta)[43] = 0;
		return 0;
	case 0x4f: return -M_ENMFILES;
	case 0x56: return RV(rename(hpath(P(4), h), hpath(P(8), h2)));
	case 0x57:
		if (W(8) == 0) {
			if (fstat(W(6), &sb) < 0)
				return merr(errno);
			v = dosdt(sb.st_mtime);
			PUT16(P(2), v), PUT16(P(2) + 2, v >> 16);
		}
		return 0;
	case 0xff: return 0;
	case 0x100: {
		int p[2];

		if (pipe(p) < 0)
			return merr(errno);
		PUT16(P(2), p[0]), PUT16(P(2) + 2, p[1]);
		return 0;
	}
	case 0x101: return RV(fchown(W(2), W(4), W(6)));
	case 0x102: return RV(fchmod(W(2), UW(4)));
	case 0x103: return RV(fsync(W(2)));
	case 0x104: return fcntl1(W(2), L(4), W(8));
	case 0x105:
		if (ioctl(W(2), FIONREAD, &r) < 0)
			return merr(errno);
		return r;
	case 0x106: return 1;
	case 0x109: return pwait(-1, 0, (long *)0);
	case 0x10a: return RV(nice(W(2)));
	case 0x10b: return getpid();
	case 0x10c: return getppid();
	case 0x10d: return getpgrp();
	case 0x10e: return RV(setpgid(W(2), W(4)));
	case 0x10f: return getuid();
	case 0x110: return RV(setuid(UW(2)));
	case 0x111:
		if (W(4) && !hsig(W(4)))
			return -M_EINVAL;
		return RV(kill(W(2), W(4) ? hsig(W(4)) : 0));
	case 0x112:
		v = msa[W(2) & 31].h;
		r = sigact(W(2), L(4), 0L, 0);
		return r < 0 ? r : v;
	case 0x113: case 0x11b: return RV(fork());
	case 0x114: return getgid();
	case 0x115: return RV(setgid(UW(2)));
	case 0x116: case 0x117:
		hmask((unsigned long)L(2), &ss);
		if (sigprocmask(op == 0x116 ? SIG_BLOCK : SIG_SETMASK, &ss, &os) < 0)
			return merr(errno);
		return mmask(&os);
	case 0x118: return 0;
	case 0x119: v = domain; if (W(2) >= 0) domain = W(2); return v;
	case 0x11a: sigprocmask(SIG_SETMASK, &prevmask, (sigset_t *)0); return 0;
	case 0x11c: return pwait(-1, W(2), (long *)L(4));
	case 0x11d: return fselect(UW(2), (long *)L(4), (long *)L(8), (long *)L(12));
	case 0x11e: memset(P(2), 0, 32); return 0;
	case 0x11f: return 0;
	case 0x120:
		if (L(2) < 0) {
			if ((r = alarm(0)) != 0)
				alarm(r);
			return r;
		}
		return alarm(L(2));
	case 0x121: pause(); return -128;
	case 0x122:
		switch (W(2)) {
		case -1: return 4;
		case 0: return 1024;
		case 1: return 126;
		case 2: return NFD;
		case 3: return 16;
		case 4: return 0x7fff;
		}
		return -M_EINVAL;
	case 0x123: sigpending(&ss); return mmask(&ss);
	case 0x124:
		switch (W(6)) {
		case -1: return 7;
		case 0: return NFD;
		case 1: return 32767;
		case 2: return 1024;
		case 3: return 255;
		case 4: return 4096;
		case 5: return 2;
		case 6: return 0;
		case 7: return 0x1ff;
		}
		return -M_EINVAL;
	case 0x127: return 0;
	case 0x128: return dopen(P(2), W(6));
	case 0x129: return dread(W(2), L(4), P(8));
	case 0x12a: return dget(L(2)) ? (rewinddir(dget(L(2))), 0) : -M_EBADF;
	case 0x12b:
		if (!dget(L(2)))
			return -M_EBADF;
		closedir(dget(L(2)));
		dirs[L(2) & 0xffff] = 0;
		return 0;
	case 0x12c: case 0x14b:
		hpath(P(4), h);
		if ((W(2) ? lstat(h, &sb) : stat(h, &sb)) < 0)
			return merr(errno);
		if (op == 0x12c)
			xattr(&sb, P(8));
		else
			st64(&sb, P(8));
		return 0;
	case 0x15d:
		if (fstat(W(2), &sb) < 0)
			return merr(errno);
		st64(&sb, P(4));
		return 0;
	case 0x12d: return RV(link(hpath(P(2), h), hpath(P(6), h2)));
	case 0x12e: return RV(symlink(P(2)[0] == 'u' && P(2)[1] == ':' ? hpath(P(2), h) : P(2),
	    hpath(P(6), h2)));
	case 0x12f:
		if (W(2) <= 0)
			return -M_ERANGE;
		if ((r = readlink(hpath(P(8), h), P(4), W(2) - 1)) < 0)
			return merr(errno);
		P(4)[r] = 0;
		return 0;
	case 0x131: return RV(chown(hpath(P(2), h), W(6), W(8)));
	case 0x132: return RV(chmod(hpath(P(2), h), UW(6)));
	case 0x133: return umask(UW(2));
	case 0x136:
		hmask((unsigned long)L(2), &ss);
		sigsuspend(&ss);
		return -128;
	case 0x137:
		lp = (long *)L(8);
		if (lp) {
			lp[0] = msa[W(2) & 31].h;
			lp[1] = msa[W(2) & 31].mask;
			*(short *)(lp + 2) = msa[W(2) & 31].flags;
		}
		if (!L(4))
			return 0;
		lp = (long *)L(4);
		return sigact(W(2), lp[0], lp[1], *(short *)(lp + 2));
	case 0x138: return geteuid();
	case 0x139: return getegid();
	case 0x13a: return pwait(W(2), W(4), (long *)L(6));
	case 0x13d:
		memset((char *)&it, 0, sizeof it);
		it.it_value.tv_sec = L(2) / 1000;
		it.it_value.tv_usec = L(2) % 1000 * 1000;
		setitimer(ITIMER_REAL, &it, &oit);
		return oit.it_value.tv_sec * 1000 + oit.it_value.tv_usec / 1000;
	case 0x13f:
		lp = (long *)L(2);
		if (lp) *lp = time((time_t *)0);
		lp = (long *)L(6);
		if (lp) lp[0] = lp[1] = lp[2] = 0;
		return 0;
	case 0x142: return -M_EINVFN;
	case 0x150: sync(); return 0;
	case 0x154:
		switch (W(2)) {
		case -1: return 0;
		case 0: return 0x4d694e54L;		/* 'MiNT' */
		case 2: return 0x01130000L;
		case 8:
			for (lp = *(long **)0x5a0; lp && lp[0]; lp += 2)
				if (lp[0] == L(4)) {
					if (L(8))
						*(long *)L(8) = lp[1];
					return L(8) ? 0 : lp[1];
				}
			return -1;
		}
		return -M_EINVFN;
	case 0x155:
		gettimeofday(&tv, (struct timezone *)0);
		if (L(2))
			PUT32(P(2), tv.tv_sec), PUT32(P(2) + 4, tv.tv_usec);
		if (L(6)) {
			now = tv.tv_sec;
			tm = localtime(&now);
			PUT32(P(6), timezone / 60), PUT32(P(6) + 4, tm->tm_isdst > 0);
		}
		return 0;
	case 0x15a: return fpoll(P(2), (unsigned long)L(6), (unsigned long)L(10));
	case 0x15b: case 0x15c: {
		long t = 0, k;
		char *iv = P(4);

		for (r = 0; r < L(8); r++) {
			k = mint_rw(op == 0x15c, W(2), *(char **)(iv + 8 * r), *(long *)(iv + 8 * r + 4));
			if (k < 0)
				return t ? t : k;
			t += k;
			if (k < *(long *)(iv + 8 * r + 4))
				break;
		}
		return t;
	}
	}
	if (op >= 0x160 && op <= 0x16e)
		return sockcall(op, a);
	if (trace)
		fprintf(stderr, "mintrun: no call $%x\n", op);
	return -M_EINVFN;
}

long
mint_rw(rd, fd, buf, n)
	int rd, fd;
	char *buf;
	long n;
{
	if (fd >= 0 && fd < NFD && sock[fd])
		return sk(rd ? TSO_RECV : TSO_SEND, fd, 0L, buf, n, (char *)0, (long *)0);
	return RV(rd ? read(fd, buf, n) : write(fd, buf, n));
}

long
mint_bios(a)
	char *a;
{
	char c;
	long v;

	switch (UW(0)) {
	case 1: return -1;
	case 2: return read(0, &c, 1) == 1 ? (unsigned char)c : 0;
	case 3: c = W(4); write(W(2) == 2 ? 1 : 2, &c, 1); return 0;
	case 5:
		if (W(2) < 0 || W(2) > 255)
			return 0;
		v = *(long *)(W(2) * 4L);
		if (L(4) != -1)
			*(long *)(W(2) * 4L) = L(4);
		return v;
	case 6: return 5;
	case 8: return -1;
	case 10: return 1L << 2 | 1L << 20;
	case 11: return 0;
	}
	return 0;
}

long
mint_xbios(a)
	char *a;
{
	switch (UW(0)) {
	case 17: return rand() & 0xffffff;
	case 38: return (*(long (*)())L(2))();
	}
	return 0;
}

/* ---- loading ---- */

static unsigned long
loadprg(path)
	char *path;
{
	unsigned char hd[28], *t, *r;
	unsigned long tl, dl, bl, sl, off, fix, n;
	int fd;
	char *bp = (char *)BP;
	struct stat sb;

	if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &sb) < 0)
		die(path);
	if (read(fd, (char *)hd, 28) != 28 || hd[0] != 0x60 || hd[1] != 0x1a) {
		fprintf(stderr, "mintrun: %s: not a TOS program\n", path);
		exit(126);
	}
	tl = *(long *)(hd + 2), dl = *(long *)(hd + 6), bl = *(long *)(hd + 10);
	sl = *(long *)(hd + 14);
	t = (unsigned char *)BP + 256;
	if (tl > mem || dl > mem || bl > mem || sl > mem || tl + dl < 4 ||
	    BP + 256 + tl + dl + bl + 0x20000 > mem) {
		fprintf(stderr, "mintrun: %s: too big for MINTMEM\n", path);
		exit(126);
	}
	if (read(fd, (char *)t, tl + dl) != tl + dl)
		die(path);
	lseek(fd, 28 + tl + dl + sl, 0);
	n = sb.st_size - (28 + tl + dl + sl);
	r = (unsigned char *)(BP + 256 + tl + dl);	/* the fixups go in the bss, cleared after */
	if (n >= 4 && n < mem && *(short *)(hd + 26) == 0 && (unsigned long)r + n + 1 <= mem &&
	    read(fd, (char *)r, n) == n && (fix = *(long *)r) != 0) {
		r[n] = 0;
		for (off = fix, r += 4; off <= tl + dl - 4; off += *r++) {
			*(long *)(t + off) += (long)t;
			while (*r == 1)
				off += 254, r++;
			if (!*r)
				break;
		}
	}
	close(fd);
	memset((char *)t + tl + dl, 0, bl);
	memset(bp, 0, 256);
	PUT32(bp, BP);
	PUT32(bp + 4, mem);
	PUT32(bp + 8, t);
	PUT32(bp + 12, tl);
	PUT32(bp + 16, t + tl);
	PUT32(bp + 20, dl);
	PUT32(bp + 24, t + tl + dl);
	PUT32(bp + 28, bl);
	PUT32(bp + 32, bp + 128);
	dta = (long)(bp + 128);
	brk0 = (unsigned long)t + tl + dl + bl;
	return (unsigned long)t;
}

/* the environment block, with argv as ARGV=, below the program's stack */
static unsigned long
envblock(argc, argv, top)
	int argc;
	char **argv;
	unsigned long top;
{
	char **e;
	unsigned long n = 0, p;
	int i;

	for (e = environ; *e; e++)
		if (strncmp(*e, "ARGV=", 5))
			n += strlen(*e) + 1;
	for (i = 0; i < argc; i++)
		n += strlen(argv[i]) + 1;
	if (n > top / 4) {
		fprintf(stderr, "mintrun: arguments and environment too big\n");
		exit(126);
	}
	p = (top - n - 64) & ~3;
	n = p;
	for (e = environ; *e; e++)
		if (strncmp(*e, "ARGV=", 5)) {
			strcpy((char *)n, *e);
			n += strlen(*e) + 1;
		}
	strcpy((char *)n, "ARGV=");
	n += 6;
	for (i = 0; i < argc; i++) {
		strcpy((char *)n, argv[i]);
		n += strlen(argv[i]) + 1;
	}
	*(char *)n = 0;
	return p;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct tosenter te;
	unsigned long pc, env, sp;
	char h[1024], *s, *bp = (char *)BP;
	long *jar = (long *)JAR;
	struct stat sb;
	int zfd;

	if (argc < 2) {
		fprintf(stderr, "usage: mintrun prog [args]\n");
		return 2;
	}
	if (argv[0][0] == '/')
		self = argv[0];
	if ((s = getenv("MINTRUN")) != 0)
		self = s;
	trace = (s = getenv("MINTTRACE")) != 0 ? atoi(s) > 1 ? 2 : 1 : 0;
	if ((s = getenv("MINTMEM")) != 0)
		mem = (strtoul(s, (char **)0, 0) + 0xfff) & ~0xfffUL;
	if (mem < 0x80000 || mem > 0xe00000)
		mem = 0x800000;
	if ((tfd = open("/dev/tos", O_RDWR)) < 0)
		die("/dev/tos");
	fcntl(tfd, F_SETFD, 1);
	if ((zfd = open("/dev/zero", O_RDWR)) < 0)
		die("/dev/zero");
	if (mmap((caddr_t)0, mem, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, zfd, 0) == (caddr_t)-1)
		die("mmap");
	close(zfd);
	/* a host path, unless it has a drive or backslashes; Pexec passes host paths */
	pc = loadprg(strchr(argv[1], '\\') || (argv[1][0] && argv[1][1] == ':') ?
	    hpath(argv[1], h) : argv[1]);
	PUT32((char *)0x84, mint_t1);
	PUT32((char *)0x88, mint_t2);
	PUT32((char *)0xb4, mint_t13);
	PUT32((char *)0xb8, mint_t14);
	PUT32((char *)0x5a0, jar);
	jar[0] = 0x5f435055L, jar[1] = 40;		/* _CPU */
	jar[2] = 0x5f4d4348L, jar[3] = 0x00020000L;	/* _MCH: TT */
	jar[4] = 0x4d694e54L, jar[5] = 0x113;		/* MiNT 1.19 */
	jar[6] = 0, jar[7] = 8;
	env = envblock(argc - 1, argv + 1, mem);
	PUT32(bp + 44, env);
	PUT32(bp + 4, env & ~0xfffUL);
	envlo = env & ~0xfffUL;
	bp[128] = 127;					/* the arguments are in ARGV */
	sp = env & ~3;
	te.te_ramsize = mem;
	te.te_flags = TEF_NOMACH;
	if (ioctl(tfd, TOSIOC_ENTER, &te) < 0)
		die("TOSIOC_ENTER");
	for (zfd = 0; zfd < NFD; zfd++)		/* sockets inherited through exec */
		sock[zfd] = fstat(zfd, &sb) == 0 && S_ISCHR(sb.st_mode) && issock(zfd);
	mint_go(pc, sp, BP);
	return 0;
}
