/*
 * t_stable.c -- every environment on this root started and stopped in a
 * loop, and its CPU use at rest.
 *
 *   /tests/t_stable [env ...]
 *
 * /tests/stable.envs, when present, names the environments instead and
 * may set idle=SECONDS (default 60) and cycles=N, fewer stop kinds.
 * Per environment: a start, until it settles (the machine's load back
 * near the boot baseline), then ends in turn by SIGTERM, Ctrl-C (SIGINT
 * to its group), a hangup (the login line closed), SIGKILL and, where
 * there is one, its own exit (the Mac's Special menu).  After each stop:
 * no process of the session left, its lock free, Mac RAM gone; against
 * the first stop, free memory, swap and RAM reservations, System V IPC
 * objects, file locks, the next free pty and the process count back.
 * Its guest module unloads before the last start, which loads it again.
 * After the first start, IDLE seconds at rest: the emulator's host CPU
 * above the baseline ("load") and the CPU charged to the session's
 * processes ("charged"), each at most 5%.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/procfs.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <termio.h>
#include "sys/mod.h"
#include "amigaio.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"
#define	LOG	"/tmp/stable.log"
#define	TLOW	0x744c4f57		/* 'tLOW', Mac RAM */
#define	MACVOL	"/macsys"
#define	MACDEV	"/dev/dsk/c1d0s0"
#define	ROOT6	"/a201"
#define	SYS6DIR	ROOT6 "/mac/sys/System Folder"
#define	EMUTOS	"/etc/tos/emutos.img"
#define	CDIR	"/tmp/stc"
#define	UDIR	"/tmp/stu"
#define	CPMH	"/tmp/stcpm"
#define	AMISYS	"/amiga/sys"
#define	AMIDEV	"/dev/dsk/c0d0s0"
#define	WIN	"/win"
#define	WINDEV	"/dev/dsk/c3d0s0"
#define	GATE	5			/* % at rest */

struct env {
	char	*name;
	char	*need;			/* a file the environment needs */
	char	*mod;			/* its guest module */
	int	boot;			/* s to settle */
	int	(*prep)();		/* once before; 0 skips */
	void	(*args)();		/* the child's exec */
	void	(*done)();
	char	*lock;			/* the session's lock file */
	int	(*own)();		/* its own exit; 0: none */
};

struct snap {
	long	freemem, rmem, smem, procs, shm, sem, msg, flk[1], pty;
};

static int hfd = -1, hseq, ufd = -1, cin = -1;
static char hrep[160];
static long base = -1;		/* host load at rest, per mille */
static char *sys;		/* the Mac's System Folder */
static int boxflag;
static struct env *cur;
static long idle = 60;		/* s at rest measured */
static int cycles = 5;		/* stops of each kind, at most 5 */
static char only[256];		/* environments to run, none: all */

/* ---- kernel memory ---- */

static long
kaddr(name)
	char *name;
{
	unsigned long a = 0, info;

	return getksym(name, &a, &info) < 0 ? 0 : (long)a;
}

static long
kread(a)
	long a;
{
	long v = -1;
	int fd = open("/dev/kmem", O_RDONLY);

	if (fd >= 0 && (lseek(fd, (off_t)a, 0) == -1 || read(fd, (char *)&v, 4) != 4))
		v = -1;
	if (fd >= 0)
		close(fd);
	return v;
}

static int
setsym(name, v)
	char *name;
	long v;
{
	long a = kaddr(name);
	int fd, r = -1;

	if (a && (fd = open("/dev/kmem", O_RDWR)) >= 0) {
		if (lseek(fd, (off_t)a, 0) != -1 && write(fd, (char *)&v, 4) == 4)
			r = 0;
		close(fd);
	}
	return r;
}

/* allocated IPC objects: table of n entries of size sz, mode at off */
static long
ipcs(tab, info, ioff, sz, off)
	char *tab, *info;
	int ioff, sz, off;
{
	long t = kaddr(tab), i = kaddr(info), n, k, c = 0;

	if (!t || !i || (n = kread(i + ioff)) < 0 || n > 1000)
		return -1;
	for (k = 0; k < n; k++)
		if (kread(t + k * sz + off) & 0x8000L)
			c++;
	return c;
}

/* ---- host line (display/hostio.py) ---- */

static char *
host(req)
	char *req;
{
	char buf[160], line[256], *nl, *s;
	struct pollfd p;
	int n, len = 0, seq;
	long t0 = t_now_ms();

	if (hfd < 0)
		return 0;
	sprintf(buf, "@@ %d %s\n", ++hseq, req);
	write(hfd, buf, strlen(buf));
	p.fd = hfd;
	p.events = POLLIN;
	while (t_now_ms() - t0 < 20000) {
		if (poll(&p, 1L, 500) <= 0 || (n = read(hfd, line + len, sizeof line - 1 - len)) <= 0)
			continue;
		len += n;
		line[len] = 0;
		while ((nl = strchr(line, '\n')) != 0) {
			*nl = 0;
			if ((s = strstr(line, "@@ok ")) != 0 && sscanf(s + 5, "%d", &seq) == 1 &&
			    seq == hseq) {
				s = strchr(s + 5, ' ');
				strncpy(hrep, s ? s + 1 : "", sizeof hrep - 1);
				hrep[strcspn(hrep, "\r")] = 0;
				return hrep;
			}
			if ((s = strstr(line, "@@err ")) != 0 && sscanf(s + 6, "%d", &seq) == 1 &&
			    seq == hseq)
				return 0;
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

/* the emulator's host CPU and wall clock, ms; 0 without the host */
static int
hostcpu(c, w)
	long *c, *w;
{
	char *r = host("cpu");

	return r != 0 && sscanf(r, "%ld %ld", c, w) == 2;
}

static void
nap(ms)
	long ms;
{
	poll((struct pollfd *)0, 0L, (int)ms);
}

/* ---- the session's processes ---- */

/*
 * The processes of the session led by pid: its group or session, or a
 * descendant.  Their charged CPU, ms, in *ms; the count is returned.
 */
static int
procs(pid, ms, say)
	pid_t pid;
	long *ms;
	int say;
{
	static pid_t in[256], par[256], all[256];
	static long t[256], wc[256];
	static char nm[256][17], st[256];
	prpsinfo_t ps;
	struct dirent *de;
	char path[40];
	DIR *d;
	int fd, n = 0, k, i, j, more;

	if (ms)
		*ms = 0;
	if ((d = opendir("/proc")) == 0)
		return -1;
	while ((de = readdir(d)) != 0 && n < 256) {
		if (de->d_name[0] == '.')
			continue;
		sprintf(path, "/proc/%s", de->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &ps) == 0 && !ps.pr_zomb) {
			all[n] = ps.pr_pid;
			par[n] = ps.pr_ppid;
			in[n] = ps.pr_pid == pid || ps.pr_pgrp == pid || ps.pr_sid == pid;
			t[n] = ps.pr_time.tv_sec * 1000L + ps.pr_time.tv_nsec / 1000000;
			wc[n] = (long)ps.pr_wchan;
			st[n] = ps.pr_sname;
			memcpy(nm[n], ps.pr_fname, 16);
			nm[n][16] = 0;
			n++;
		}
		close(fd);
	}
	closedir(d);
	do {
		more = 0;
		for (i = 0; i < n; i++)
			for (j = 0; !in[i] && j < n; j++)
				if (in[j] && par[i] == all[j])
					in[i] = more = 1;
	} while (more);
	for (i = k = 0; i < n; i++)
		if (in[i]) {
			k++;
			if (ms)
				*ms += t[i];
			if (say)
				t_info("proc", "%s pid %ld %s %ld ms, %c wchan %lx", cur->name, (long)all[i],
				    nm[i], t[i], st[i], wc[i]);
		}
	return k;
}

static long
nprocs()
{
	struct dirent *de;
	DIR *d = opendir("/proc");
	long n = 0;

	while (d && (de = readdir(d)) != 0)
		n += de->d_name[0] != '.';
	if (d)
		closedir(d);
	return n;
}

/* the index of the next free pty, -1 unknown */
static long
nextpty()
{
	extern char *ptsname();
	char *s;
	long n = -1;
	int m = open("/dev/ptmx", O_RDWR);

	if (m >= 0 && (s = ptsname(m)) != 0 && (s = strrchr(s, '/')) != 0)
		n = atol(s + 1);
	if (m >= 0)
		close(m);
	return n;
}

static void
snap(s)
	struct snap *s;
{
	long f = kaddr("flckinfo");

	s->freemem = t_kmem("freemem");
	s->rmem = t_kmem("availrmem");
	s->smem = t_kmem("availsmem");
	s->procs = nprocs();
	s->shm = ipcs("shmem", "shminfo", 8, (int)sizeof(struct shmid_ds),
	    (int)(long)&((struct shmid_ds *)0)->shm_perm.mode);
	s->sem = ipcs("sema", "seminfo", 4, (int)sizeof(struct semid_ds),
	    (int)(long)&((struct semid_ds *)0)->sem_perm.mode);
	s->msg = ipcs("msgque", "msginfo", 12, (int)sizeof(struct msqid_ds),
	    (int)(long)&((struct msqid_ds *)0)->msg_perm.mode);
	/* record locks in use */
	s->flk[0] = f ? kread(f) : -1;
	s->pty = nextpty();
}

/* pid holding a write lock on path, 0 none, -1 no file */
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
	fl.l_type = fcntl(fd, F_GETLK, &fl) < 0 ? F_UNLCK : fl.l_type;
	close(fd);
	return fl.l_type == F_UNLCK ? 0 : (long)fl.l_pid;
}

static int
module(name)
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

static void
wfile(p, s)
	char *p, *s;
{
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd >= 0) {
		write(fd, s, strlen(s));
		close(fd);
	}
}

static void
fcopy(a, b)
	char *a, *b;
{
	char buf[8192];
	int i = open(a, O_RDONLY), o = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644), n;

	while (i >= 0 && o >= 0 && (n = read(i, buf, sizeof buf)) > 0)
		write(o, buf, n);
	if (i >= 0)
		close(i);
	if (o >= 0)
		close(o);
}

/* copy the tree a to b; b 0 removes a */
static void
tree(a, b)
	char *a, *b;
{
	char x[256], y[256];
	struct dirent *de;
	struct stat sb;
	DIR *d;

	if (b)
		mkdir(b, 0755);
	if ((d = opendir(a)) == 0) {
		if (!b)
			unlink(a);
		return;
	}
	while ((de = readdir(d)) != 0) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		sprintf(x, "%s/%s", a, de->d_name);
		if (b)
			sprintf(y, "%s/%s", b, de->d_name);
		if (lstat(x, &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR)
			tree(x, b ? y : (char *)0);
		else if (b)
			fcopy(x, y);
		else
			unlink(x);
	}
	closedir(d);
	if (!b)
		rmdir(a);
}

static int
reg(name, type, data)
	char *name;
	int type;
	caddr_t data;
{
	struct mod_mreg r;

	strcpy(r.md_modname, name);
	r.md_typedata = data;
	return modadm(type, MOD_C_MREG, &r) == 0 || errno == EEXIST;
}

static void
vmem(mb)
	long mb;
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < mb << 20) {
		rl.rlim_cur = rl.rlim_max < mb << 20 ? rl.rlim_max : mb << 20;
		setrlimit(RLIMIT_VMEM, &rl);
	}
}

/* ---- Mac ---- */

static char *sys6sp[] = { "%AUX Resources", "DA Handler", "Key Layout", "Scrapbook File", 0 };

static void
sys6links(on)
	int on;
{
	char a[80], b[80], *s, *d;
	int i;

	for (i = 0; sys6sp[i]; i++) {
		sprintf(b, "%s/%s", SYS6DIR, sys6sp[i]);
		for (s = sys6sp[i], d = a + sprintf(a, "%s/", SYS6DIR); *s; s++)
			if (*s != ' ')
				*d++ = *s;
		*d = 0;
		if (on)
			link(a, b);
		else
			unlink(b);
	}
}

/* A/UX's File ID daemon and mount table, once */
static void
fidd()
{
	static int done;
	struct stat sb, mv;
	FILE *f;
	pid_t p;

	if (done++ || stat("/etc/aux/fidd", &sb) < 0)
		return;
	if (stat("/", &sb) == 0 && (f = fopen("/etc/mtab", "w")) != 0) {
		fprintf(f, "/dev/dsk/c0d0s0 / 4.2 rw,noquota,dev=%x 1 1\n",
		    (int)(major(sb.st_dev) << 8 | minor(sb.st_dev)));
		if (stat(MACVOL "/S761", &mv) == 0 && mv.st_dev != sb.st_dev)
			fprintf(f, MACDEV " " MACVOL " 4.2 rw,noquota,dev=%x 1 1\n",
			    (int)(major(mv.st_dev) << 8 | minor(mv.st_dev)));
		fclose(f);
	}
	if ((p = fork()) == 0) {
		setpgrp();
		execl("/etc/aux/fidd", "fidd", "-d", (char *)0);
		_exit(127);
	}
	t_waitchild(p, (int *)0, 10);
}

static int
macprep(e)
	struct env *e;
{
	static struct mod_execreg er;
	static int mj = 54, once;
	struct stat sb;

	if (!once++) {
		modpath(MD);
		er.er_magic = 0x150;
		er.er_flags = EXF_FIRST;
		if (!reg("auxexec", MOD_TY_EXEC, (caddr_t)&er) ||
		    !reg("uinter", MOD_TY_CDEV, (caddr_t)&mj))
			return 0;
		if (stat(MACDEV, &sb) == 0 && stat(MACVOL "/S761", &sb) < 0)
			system("/sbin/mount -F ufs " MACDEV " " MACVOL);
		mkdir("/Desktop Folder", 0777);
		vmem(48L);
	}
	fidd();
	if (stat(e->need, &sb) < 0)
		return 0;
	sys = e->need;
	boxflag = 0;
	if (strcmp(e->name, "mac6") == 0) {
		boxflag = 5;
		if (rename(e->need, SYS6DIR) < 0)
			return 0;
		sys6links(1);
		sys = "/mac/sys/System Folder";
	} else if (strncmp(e->need, MACVOL, strlen(MACVOL)) == 0)
		boxflag = 29;
	if (boxflag) {
		ufd = open("/dev/uinter0", O_RDWR);
		setsym("uinter_boxflag", (long)boxflag);
	}
	return 1;
}

static void
macdone(e)
	struct env *e;
{
	if (strcmp(e->name, "mac6") == 0) {
		sys6links(0);
		rename(SYS6DIR, e->need);
	}
	if (boxflag)
		setsym("uinter_boxflag", -1L);
	if (ufd >= 0)
		close(ufd);
	ufd = -1;
}

static void
macexec(e)
	struct env *e;
{
	static char *av[] = { "/mac/bin/startmac", 0 };
	char *env[6], tb[80], *mem;

	mem = strcmp(e->name, "mac81") == 0 ? "TBMEMORY=32M" :
	    strcmp(e->name, "mac76") == 0 ? "TBMEMORY=16M" :
	    strcmp(e->name, "mac6") == 0 ? "TBMEMORY=4M" : "TBMEMORY=8M";
	sprintf(tb, "TBSYSTEM=%s", sys);
	env[0] = "PATH=/aux/bin:/usr/bin:/sbin";
	env[1] = "HOME=/tmp";
	env[2] = tb;
	env[3] = mem;
	env[4] = 0;
	system("/usr/bin/rm -rf /tmp/.mac");
	if (strcmp(e->name, "mac6") == 0 && (chroot(ROOT6) < 0 || chdir("/") < 0))
		_exit(126);
	execve(av[0], av, env);
}

/* the mouse to h, v on the Mac's screen; 1 when there */
static int
moveto(h, v)
	int h, v;
{
	char r[40];
	long a = kaddr("uin_mouse"), m;
	int i, dh, dv;

	for (i = 0; a && i < 30; i++) {
		m = kread(a);
		dh = h - (short)m;
		dv = v - (short)(m >> 16);
		if (dh >= -2 && dh <= 2 && dv >= -2 && dv <= 2)
			return 1;
		dh = dh / 2 ? dh / 2 : dh;
		dv = dv / 2 ? dv / 2 : dv;
		dh = dh > 40 ? 40 : dh < -40 ? -40 : dh;
		dv = dv > 40 ? 40 : dv < -40 ? -40 : dv;
		sprintf(r, "move %d %d", dh, dv);
		host(r);
		nap(100L);
	}
	return 0;
}

/* Special > Shut Down: without A/UX's dialog resource, a Log Out */
static int
macout(e)
	struct env *e;
{
	int h = 232, sh = 250, sv = 139;

	if (strcmp(e->name, "mac81") == 0)
		h = 181, sh = 200, sv = 109;
	if (!moveto(h, 9))
		return 0;
	host("button 1");
	nap(800L);
	moveto(sh, sv);
	nap(500L);
	host("button 0");
	return 1;
}

/* ---- TOS ---- */

static int
tosprep(e)
	struct env *e;
{
	static int mj = 56;
	struct stat sb;

	modpath(MD);
	if (!reg("tosguest", MOD_TY_CDEV, (caddr_t)&mj) || stat(EMUTOS, &sb) < 0 ||
	    stat(e->need, &sb) < 0)
		return 0;
	tree(CDIR, (char *)0);
	tree(UDIR, (char *)0);
	mkdir(UDIR, 0755);
	if (strcmp(e->name, "emudesk") == 0)
		mkdir(CDIR, 0755);
	else if (strcmp(e->name, "teradesk") == 0) {
		char b[2048];
		int fd, n = 0;

		tree("/tos/fvdi", CDIR);
		tree("/tos/teradesk/TERADESK", CDIR "/TERADESK");
		if ((fd = open("/tos/teradesk/EMUDESK.INF", O_RDONLY)) >= 0) {
			n = read(fd, b, sizeof b - 1);
			close(fd);
		}
		b[n > 0 ? n : 0] = 0;
		wfile(CDIR "/EMUDESK.INF", b);
	} else {
		tree("/tos/xaaes", CDIR);
		tree("/tos/fvdi", CDIR);
		tree("/tos/teradesk/TERADESK", CDIR "/TERADESK");
		mkdir(CDIR "/CLIPBRD", 0755);
	}
	return 1;
}

static void
tosexec(e)
	struct env *e;
{
	putenv("HOME=/tmp");
	execl("/tos/bin/starttos", "starttos", "-rom", EMUTOS, "-u", UDIR, "-C", CDIR, (char *)0);
}

static void
tosdone(e)
	struct env *e;
{
	tree(CDIR, (char *)0);
	tree(UDIR, (char *)0);
}

/* ---- Amiga ---- */

static int
amiprep(e)
	struct env *e;
{
	static int mj = 57;
	struct stat sb;
	int fd;

	modpath(MD);
	if (stat(e->need, &sb) < 0 || !reg("amigaguest", MOD_TY_CDEV, (caddr_t)&mj) ||
	    (fd = open(AMIDEV, O_RDONLY)) < 0)
		return 0;
	close(fd);
	vmem(128L);
	return system("/sbin/mount -F ufs " AMIDEV " " AMISYS) == 0;
}

static void
amiexec(e)
	struct env *e;
{
	putenv("HOME=/nonexistent");
	execl("/tests/amiga/startmig", "startmig", "-m", "16", "-boot", "/etc/amiga/boot.rom",
	    (char *)0);
}

static void
amidone(e)
	struct env *e;
{
	int i;

	for (i = 0; i < 10 && system("/sbin/umount " AMISYS) != 0; i++)
		sleep(1);
}

/* ---- CP/M ---- */

static int
cpmprep(e)
	struct env *e;
{
	static int mj = 59;
	struct stat sb;

	modpath(MD);
	if (stat(e->need, &sb) < 0 || !reg("cpmguest", MOD_TY_CDEV, (caddr_t)&mj))
		return 0;
	tree(CPMH, (char *)0);
	mkdir(CPMH, 0755);
	mkdir(CPMH "/CPM", 0755);
	mkdir(CPMH "/CPM/t", 0755);
	mkdir(CPMH "/c", 0755);
	return 1;
}

static void
cpmexec(e)
	struct env *e;
{
	putenv("HOME=" CPMH);
	execl("/cpm/bin/startcpm", "startcpm", "-e", "t", "-d", "C=" CPMH "/c", (char *)0);
}

static void
cpmdone(e)
	struct env *e;
{
	tree(CPMH, (char *)0);
}

/* ---- Win16 ---- */

static int
winprep(e)
	struct env *e;
{
	char *av[12], path[6][40];
	struct stat sb;
	int i, n = 0, st = -1;
	pid_t p;
	long t0;

	if (stat(e->need, &sb) < 0 || stat(WINDEV, &sb) < 0)
		return 0;
	if (stat(WIN "/home", &sb) < 0 && system("/sbin/mount -F ufs " WINDEV " " WIN) != 0)
		return 0;
	if (access(WIN "/home/WIN16/windows/progman.exe", 0) == 0)
		return 1;
	if (access(WIN "/disks/DISK1.IMG", 0) < 0)
		return 0;
	av[n++] = "startwin";
	av[n++] = "-install";
	for (i = 0; i < 6; i++) {
		sprintf(path[i], WIN "/disks/DISK%d.IMG", i + 1);
		av[n++] = path[i];
	}
	av[n++] = WIN "/wabi";
	av[n] = 0;
	t0 = t_now_ms();
	t_rearm(1500);
	if ((p = fork()) == 0) {
		putenv("HOME=" WIN "/home");
		i = open("/dev/null", O_RDWR);
		dup2(i, 1);
		dup2(i, 2);
		execv(e->need, av);
		_exit(126);
	}
	t_waitchild(p, &st, 1200);
	t_info("win16_install", "%ld ms, status %#x", t_now_ms() - t0, st);
	return access(WIN "/home/WIN16/windows/progman.exe", 0) == 0;
}

static void
winexec(e)
	struct env *e;
{
	putenv("HOME=" WIN "/home");
	execl(e->need, "startwin", (char *)0);
}

/* ---- the loop ---- */

static struct env envs[] = {
	{ "mac7", "/mac/sys/Sys7", "uinter", 200, macprep, macexec, macdone, 0, 0 },
	{ "mac6", ROOT6 "/mac/sys/Sys6", "uinter", 200, macprep, macexec, macdone, 0, 0 },
	{ "mac76", MACVOL "/S761", "uinter", 300, macprep, macexec, macdone, 0, macout },
	{ "mac81", MACVOL "/S81", "uinter", 400, macprep, macexec, macdone, 0, macout },
	{ "emudesk", "/tos/bin/starttos", "tosguest", 150, tosprep, tosexec, tosdone, CDIR "/.env", 0 },
	{ "teradesk", "/tos/teradesk/TERADESK/DESKTOP.PRG", "tosguest", 150, tosprep, tosexec,
	    tosdone, CDIR "/.env", 0 },
	{ "xaaes", "/tos/xaaes/MINT", "tosguest", 200, tosprep, tosexec, tosdone, CDIR "/.env", 0 },
	{ "amiga", "/tests/amiga/startmig", "amigaguest", 150, amiprep, amiexec, amidone, 0, 0 },
	{ "cpm", "/cpm/bin/startcpm", "cpmguest", 90, cpmprep, cpmexec, cpmdone,
	    CPMH "/CPM/t/.env", 0 },
	{ "win16", "/usr/bin/startwin", 0, 400, winprep, winexec, 0, 0, 0 },
	{ 0 }
};

static pid_t
start(e)
	struct env *e;
{
	int p[2], fd;
	pid_t pid;

	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) == 0) {
		setpgrp();
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		dup2(p[0], 0);
		fd = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		for (fd = 3; fd < 64; fd++)
			close(fd);
		e->args(e);
		_exit(127);
	}
	close(p[0]);
	cin = p[1];
	return pid;
}

/* the last lines of the session's output */
static void
relay()
{
	char l[200];
	FILE *f = fopen(LOG, "r");
	long n = 0, k = 0;

	while (f && fgets(l, sizeof l, f))
		n++;
	if (f)
		rewind(f);
	while (f && fgets(l, sizeof l, f))
		if (++k > n - 12)
			printf("%s| %s", cur->name, l);
	if (f)
		fclose(f);
	fflush(stdout);
}

/*
 * Over ms: the emulator's host CPU above the baseline and the CPU
 * charged to the session, per mille; host -1 without the host line.
 */
static void
load(pid, ms, hl, cl)
	pid_t pid;
	long ms, *hl, *cl;
{
	long c0, w0, c1, w1, p0, p1, t0, dt;
	int h;

	procs(pid, &p0, 0);
	h = hostcpu(&c0, &w0);
	t0 = t_now_ms();
	nap(ms);
	h = hostcpu(&c1, &w1) && h;
	procs(pid, &p1, 0);
	dt = t_now_ms() - t0;
	*cl = dt > 0 ? (p1 - p0) * 1000 / dt : -1;
	*hl = h && w1 > w0 ? (c1 - c0) * 1000 / (w1 - w0) - (base > 0 ? base : 0) : -1;
}

/* seconds until the session's load stays at GATE for 10 s, -1 if it never does */
static long
settle(e, pid)
	struct env *e;
	pid_t pid;
{
	long t0 = t_now_ms(), hl, cl;
	int quiet = 0;

	nap(10000L);
	while (t_now_ms() - t0 < e->boot * 1000L) {
		if (waitpid(pid, (int *)0, WNOHANG) == pid)
			return -2;
		load(pid, 5000L, &hl, &cl);
		t_info(e->name, "load %ld charged %ld per mille", hl, cl);
		if ((hl >= 0 ? hl : cl) > GATE * 10)
			quiet = 0;
		else if (++quiet == 2)
			return (t_now_ms() - t0) / 1000;
	}
	return -1;
}

static int
gone(e, pid, how)
	struct env *e;
	pid_t pid;
	int how;
{
	int st = 0, sig[] = { SIGTERM, SIGINT, SIGHUP, SIGKILL };
	long t0 = t_now_ms();
	pid_t r = 0;

	if (how == 4) {
		if (!e->own(e)) {
			t_info(e->name, "own exit not reached, SIGKILL");
			kill(-pid, SIGKILL);
		}
	} else if (how == 0)
		kill(pid, SIGTERM);
	else
		kill(-pid, sig[how]);
	while (t_now_ms() - t0 < 60000L && (r = waitpid(pid, &st, WNOHANG)) == 0)
		nap(250L);
	if (r != pid) {
		procs(pid, (long *)0, 1);
		kill(-pid, SIGKILL);
		t_waitchild(pid, &st, 10);
	}
	close(cin);
	cin = -1;
	t0 = t_now_ms();
	while (procs(pid, (long *)0, 0) > 0 && t_now_ms() - t0 < 10000L)
		nap(250L);
	t_info(e->name, "stop %d status %#x after %ld ms", how, st, t_now_ms() - t0);
	return r == pid;
}

/* the Mac's event calls by outcome, over the idle time */
static void
evstats(end)
	int end;
{
	static long v[8];
	long a = kaddr("uin_evstats");
	int i;

	if (!a)
		return;
	setsym("uin_evdiag", end ? 0L : 1L);
	for (i = 0; i < 8; i++)
		v[i] = end ? kread(a + 4 * i) - v[i] : kread(a + 4 * i);
	if (end)
		t_info(cur->name, "events %ld %ld %ld %ld %ld %ld %ld %ld", v[0], v[1], v[2], v[3],
		    v[4], v[5], v[6], v[7]);
}

/* the Amiga guest's traps over the idle time: privileged, faults, interrupts, stops */
static void
amistats(pid, end)
	pid_t pid;
	int end;
{
	static struct amigastat a0;
	struct amigastat a;
	struct amigawait aw;
	int fd = open("/dev/amiga", O_RDWR), g;

	for (g = pid; fd >= 0 && g < pid + 16; g++) {
		memset((char *)&aw, 0, sizeof aw);
		aw.aw_pid = g;
		aw.aw_stat = &a;
		if (ioctl(fd, AMIGAIOC_GSTAT, &aw) == 0)
			break;
	}
	if (fd >= 0)
		close(fd);
	if (fd < 0 || g == pid + 16)
		return;
	if (end > 1) {
		static unsigned long ad[16], pc[16];
		static int nn[16];
		int i, k, n = 0;

		fd = open("/dev/amiga", O_RDWR);
		for (k = 0; fd >= 0 && k < 400; k++) {
			nap(10L);
			if (ioctl(fd, AMIGAIOC_GSTAT, &aw) < 0 || a.as_fault == a0.as_fault)
				continue;
			a0 = a;
			for (i = 0; i < n && (ad[i] != a.as_lastaddr || pc[i] != a.as_lastpc); i++)
				;
			if (i == n && n < 16) {
				ad[n] = a.as_lastaddr;
				pc[n] = a.as_lastpc;
				nn[n++] = 0;
			}
			if (i < 16)
				nn[i]++;
		}
		if (fd >= 0)
			close(fd);
		for (i = 0; i < n; i++)
			t_info(cur->name, "fault at %lx from %lx: %d", ad[i], pc[i], nn[i]);
		return;
	}
	if (end)
		t_info(cur->name, "guest %d: priv %lu fault %lu intr %lu stop %lu, pc %lx", g,
		    a.as_priv - a0.as_priv, a.as_fault - a0.as_fault, a.as_intr - a0.as_intr,
		    a.as_stop - a0.as_stop, a.as_lastpc);
	a0 = a;
}

static char *how[] = { "term", "int", "hup", "kill", "own" };

static void
run(e)
	struct env *e;
{
	struct snap b0, b1, s;
	char n[48], w[64];
	long hl, cl, t, sv = -1, h;
	int c, k, nc = e->own ? 5 : 4, id, ok;
	pid_t pid;

	cur = e;
	if (cycles < nc)
		nc = cycles;
	if (!e->prep(e)) {
		t_skip(e->name, "not on this root");
		return;
	}
	snap(&b0);
	t_info(e->name, "baseline free %ld rmem %ld smem %ld procs %ld shm %ld sem %ld msg %ld "
	    "flck %ld pty %ld", b0.freemem, b0.rmem, b0.smem, b0.procs, b0.shm, b0.sem,
	    b0.msg, b0.flk[0], b0.pty);
	/* a last start after the module's unload loads it again */
	for (c = 0; c <= nc; c++) {
		k = c < nc ? c : 0;
		t_rearm(e->boot + 300);
		if (c == nc && e->mod && (id = module(e->mod)) > 0) {
			sprintf(n, "%s_unload", e->name);
			if (ufd >= 0)
				close(ufd);
			ok = moduload(id);
			t_check(n, ok == 0, "%s: %s", e->mod, T_ERR);
			if (ufd >= 0) {
				ufd = open("/dev/uinter0", O_RDWR);
				setsym("uinter_boxflag", (long)boxflag);
			}
		}
		pid = start(e);
		if (e->args == macexec)
			evstats(0);
		t = settle(e, pid);
		if (e->args == macexec && t >= 0)
			evstats(1);
		sprintf(n, "%s_start%d", e->name, c);
		if (!t_check(n, t >= 0, t == -2 ? "exited while starting" :
		    "busy after %d s", e->boot)) {
			if (e->mod && (id = module(e->mod)) > 0) {
				struct modstatus ms;

				modstat(id, &ms, 1);
				t_info(e->name, "%s at %lx", e->mod, (long)ms.ms_base);
			}
			if (e->args == macexec)
				evstats(1);
			sprintf(w, "prof 20 %s_busy%d", e->name, c);
			host(w);
			sprintf(w, "shot %s_busy%d", e->name, c);
			nap(21000L);
			host(w);
			relay();
			procs(pid, (long *)0, 1);
		}
		if (t >= 0) {
			t_info(n, "settled after %ld s", t);
			sprintf(w, "shot %s", n);
			host(w);
		}
		if (c == 0 && t != -2) {
			t_rearm(600);
			if (e->args == macexec)
				evstats(0);
			procs(pid, (long *)0, 1);
			if (e->args == amiexec)
				amistats(pid, 0);
			load(pid, idle * 1000L, &hl, &cl);
			if (e->args == amiexec) {
				amistats(pid, 1);
				amistats(pid, 2);
			}
			if (e->args == macexec)
				evstats(1);
			procs(pid, (long *)0, 1);
			sprintf(w, "shot %s_idle", e->name);
			host(w);
			t_info(e->name, "idle load %ld.%ld%% charged %ld.%ld%%", hl / 10, (hl < 0 ? -hl : hl) % 10,
			    cl / 10, cl % 10);
			sprintf(n, "%s_idle_load", e->name);
			if (hl != -1 || base > 0)
				t_check(n, hl <= GATE * 10, "%ld per mille", hl);
			sprintf(n, "%s_idle_charged", e->name);
			t_check(n, cl >= 0 && cl <= GATE * 10, "%ld per mille", cl);
		}
		ok = gone(e, pid, k);
		sprintf(n, "%s_stop_%s%d", e->name, how[k], c);
		t_check(n, ok && procs(pid, (long *)0, 0) == 0, "%d processes left",
		    procs(pid, (long *)0, 1));
		if (e->lock) {
			sprintf(n, "%s_unlock%d", e->name, c);
			h = holder(e->lock);
			t_check(n, h <= 0, "held by %ld", h);
		}
		sprintf(n, "%s_macram%d", e->name, c);
		if (e->args == macexec && !t_check(n, shmget(TLOW, 0, 0) < 0, "'tLOW' left"))
			shmctl(shmget(TLOW, 0, 0), IPC_RMID, (struct shmid_ds *)0);
		nap(2000L);
		snap(&s);
		t_info(e->name, "after %s: free %ld rmem %ld smem %ld procs %ld shm %ld sem %ld msg %ld "
		    "flck %ld pty %ld", how[k], s.freemem, s.rmem, s.smem, s.procs, s.shm,
		    s.sem, s.msg, s.flk[0], s.pty);
		if (c == 0) {
			b1 = s;
			continue;
		}
		sprintf(n, "%s_mem%d", e->name, c);
		/* a few pages either way: the buffer cache and page tables */
		t_check(n, s.freemem >= b1.freemem - 64 && s.rmem >= b1.rmem - 8 &&
		    s.smem >= b1.smem - 8, "free %ld rmem %ld smem %ld, first stop %ld %ld %ld",
		    s.freemem, s.rmem, s.smem, b1.freemem, b1.rmem, b1.smem);
		sprintf(n, "%s_ipc%d", e->name, c);
		t_check(n, s.shm == b0.shm && s.sem == b0.sem && s.msg == b0.msg,
		    "shm %ld sem %ld msg %ld, before %ld %ld %ld", s.shm, s.sem, s.msg, b0.shm,
		    b0.sem, b0.msg);
		sprintf(n, "%s_locks%d", e->name, c);
		t_check(n, s.flk[0] == b0.flk[0], "%ld in use, before %ld", s.flk[0], b0.flk[0]);
		sprintf(n, "%s_pty%d", e->name, c);
		t_check(n, s.pty == b0.pty, "next pty %ld, before %ld", s.pty, b0.pty);
		sprintf(n, "%s_procs%d", e->name, c);
		t_check(n, s.procs == b1.procs, "%ld, first stop %ld", s.procs, b1.procs);
		sv = s.freemem;
	}
	if (e->done)
		e->done(e);
	t_info(e->name, "free memory first stop %ld, last %ld pages", b1.freemem, sv);
}

/* the host load at rest, per mille */
static void
baseline()
{
	long c0, w0, c1, w1;

	if (!hostcpu(&c0, &w0))
		return;
	nap(10000L);
	if (hostcpu(&c1, &w1) && w1 > w0)
		base = (c1 - c0) * 1000 / (w1 - w0);
	t_info("baseline", "host CPU at rest %ld.%ld%%", base / 10, base % 10);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct env *e;
	char w[32], *s;
	int i, fd, n;

	t_init("stable", 900);
	if (t_kmem("guest_loading") == -1) {
		t_skip("all", "kernel has no guest support");
		return t_done();
	}
	signal(SIGPIPE, SIG_IGN);
	if ((fd = open("/tests/stable.envs", O_RDONLY)) >= 0) {
		n = read(fd, only, sizeof only - 2);
		close(fd);
		only[n > 0 ? n : 0] = 0;
		if ((s = strstr(only, "idle=")) != 0) {
			idle = atol(s + 5);
			memset(s, ' ', strcspn(s, " \n"));
		}
		if ((s = strstr(only, "cycles=")) != 0) {
			cycles = atoi(s + 7);
			memset(s, ' ', strcspn(s, " \n"));
		}
	}
	hostopen();
	baseline();
	for (e = envs; e->name; e++) {
		for (i = 1; i < argc && strcmp(argv[i], e->name) != 0; i++)
			;
		if (argc > 1 && i == argc)
			continue;
		sprintf(w, " %s ", e->name);
		for (s = only; *s; s++)
			if (*s == '\n')
				*s = ' ';
		if (argc == 1 && strspn(only, " ") != strlen(only)) {
			char pad[260];

			sprintf(pad, " %s ", only);
			if (strstr(pad, w) == 0)
				continue;
		}
		run(e);
	}
	return t_done();
}
