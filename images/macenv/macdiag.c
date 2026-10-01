/*
 * macdiag -- what the Mac task is doing, in one screen.
 *
 *	macdiag [-w]
 *
 * -w: again every 2 seconds.  Needs root (/dev/kmem, /proc).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>

extern int getksym();

#define	TLOW	0x744c4f57	/* 'tLOW', the Mac RAM segment */
#define	NCALL	16		/* UI_NCALL */

static int kfd = -1;
static char *lm;		/* Mac low memory */

/* n bytes of kernel symbol name + off; 0 if absent */
static int
kget(name, off, p, n)
	char *name;
	long off;
	char *p;
	int n;
{
	unsigned long a = 0, info;

	memset(p, 0, n);
	if (getksym(name, &a, &info) < 0)
		return 0;
	return lseek(kfd, (off_t)(a + off), 0) != -1 && read(kfd, p, n) == n;
}

static long
kl(name)
	char *name;
{
	long v;

	kget(name, 0L, (char *)&v, 4);
	return v;
}

static long
kat(addr)
	long addr;
{
	long v = 0;

	if (addr == 0 || lseek(kfd, (off_t)addr, 0) == -1 || read(kfd, (char *)&v, 4) != 4)
		return 0;
	return v;
}

#define	LW(o)	(lm ? *(unsigned short *)(lm + (o)) : 0)
#define	LL(o)	(lm ? *(unsigned long *)(lm + (o)) : 0)

/* the task: the process whose proc is ui.l_proc */
static int
findmac(pp, ps)
	long pp;
	prpsinfo_t *ps;
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	prpsinfo_t q;
	char path[32];
	int fd, pid = -1, byname = -1;

	while (d && pid < 0 && (e = readdir(d)) != 0) {
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		sprintf(path, "/proc/%s", e->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		if (ioctl(fd, PIOCPSINFO, &q) == 0) {
			if ((long)q.pr_addr == pp && pp) {
				pid = q.pr_pid;
				*ps = q;
			} else if (strcmp(q.pr_fname, "startmac") == 0 && q.pr_sname != 'Z') {
				byname = q.pr_pid;
				*ps = q;
			}
		}
		close(fd);
	}
	if (d)
		closedir(d);
	return pid >= 0 || pp == 0 ? pid : byname;
}

static void
alarmed()
{
}

/* registers; a runnable task is stopped for them and started again */
static int
regs(fd, ps, st)
	int fd;
	prpsinfo_t *ps;
	prstatus_t *st;
{
	static int sigs[] = { SIGINT, SIGQUIT, SIGHUP, SIGTERM };
	void (*h[4])();
	prrun_t run;
	int i, ok;

	if (ioctl(fd, PIOCSTATUS, st) < 0)
		return -1;
	if (ps->pr_sname != 'R' && ps->pr_sname != 'O')
		return 0;
	/* stopped by someone else: leave it; closing fd runs it if we stop it */
	if ((st->pr_flags & PR_STOPPED) || ioctl(fd, PIOCSRLC, 0) < 0)
		return -1;
	for (i = 0; i < 4; i++)
		h[i] = signal(sigs[i], SIG_IGN);
	signal(SIGALRM, alarmed);
	alarm(2);
	ok = ioctl(fd, PIOCSTOP, st) == 0;
	alarm(0);
	memset(&run, 0, sizeof run);
	ioctl(fd, PIOCRUN, &run);
	for (i = 0; i < 4; i++)
		signal(sigs[i], h[i]);
	return ok ? 1 : -1;
}

static unsigned short
uword(fd, a)
	int fd;
	long a;
{
	unsigned short w = 0;

	if (fd < 0 || lseek(fd, (off_t)a, 0) == -1 || read(fd, (char *)&w, 2) != 2)
		return 0;
	return w;
}

static char *kinds[] = { "sr", "rte", "fromsr", "tosr", "usp", "movec",
	"moves", "cache", "mmu", "fsave", "frest", "other" };

static long lastcpu = -1;

static void
show()
{
	long ui[4], g[9], npk[12], nsig[32], c[2 * NCALL], n, i, k, cpu;
	prpsinfo_t ps;
	prstatus_t st;
	char path[32], *s;
	int pid, fd = -1, how, shmid;

	kget("ui", 0L, (char *)ui, sizeof ui);
	if (!kget("ui_nposted", 0L, (char *)&n, 4)) {
		printf("macdiag: uinter not loaded\n");
		return;
	}
	pid = findmac(ui[1], &ps);
	if (pid < 0) {
		printf("macdiag: no Mac task (layer state %ld)\n", ui[0]);
		return;
	}
	sprintf(path, "/proc/%d", pid);
	fd = open(path, O_RDWR);
	cpu = ps.pr_time.tv_sec * 100 + ps.pr_time.tv_nsec / 10000000;
	printf("pid %d %.8s state %c wchan %lx cpu %ld.%02lds", pid, ps.pr_fname,
	    ps.pr_sname, (long)ps.pr_wchan, cpu / 100, cpu % 100);
	if (lastcpu >= 0)
		printf(" (+%ld.%02ld)", (cpu - lastcpu) / 100, (cpu - lastcpu) % 100);
	printf("\n");
	lastcpu = cpu;
	how = fd < 0 ? -1 : regs(fd, &ps, &st);
	if (how >= 0)
		printf("PC %08lx SR %04lx op %04lx sp %08lx %s %s\n",
		    (long)st.pr_reg[R_PC], (long)st.pr_reg[R_PS] & 0xffff,
		    (long)uword(fd, (long)st.pr_reg[R_PC]), (long)st.pr_reg[15],
		    how ? "(stopped)" : "(asleep)", st.pr_flags & PR_ASLEEP ? "in call" : "");
	else
		printf("PC: /proc failed\n");
	for (i = 0; i < 9; i++)
		g[i] = ui[0] == 2 ? kat(ui[2] + 4 * i) : 0;	/* LS_INUSE */
	printf("vSR %04lx vVBR %08lx vCACR %08lx gflags %lx\n", g[5] >> 16 & 0xffff,
	    g[7], g[8], g[3]);
	printf("ticks posted %ld taken %ld  lbolt %ld  last tick PC %08lx\n",
	    n, kl("ui_ntaken"), kl("lbolt"), kl("ui_tickpc"));

	kget("aux_nsig", 0L, (char *)nsig, sizeof nsig);
	printf("sig:");
	for (i = 1, k = 0; i < 32; i++)
		if (nsig[i] && i != SIGIOT) {
			printf(" %ld:%ld", i, nsig[i]);
			k++;
		}
	printf("%s  last exc sig %ld vec %ld pc %08lx\n", k ? "" : " none",
	    kl("aux_xsig"), kl("aux_xvec"), kl("aux_xpc"));
	printf("vec2 fmt7 %ld last pc %08lx ea %08lx\n", kl("guest_nfault7"),
	    kl("guest_f7pc"), kl("guest_f7ea"));
	k = kl("guest_lineapc");
	printf("A-line %ld last at %08lx, there now %04x\n", kl("guest_nlinea"),
	    k, uword(fd, k));
	kget("guest_npk", 0L, (char *)npk, sizeof npk);
	printf("priv:");
	for (i = 0; i < 12; i++) {
		if (npk[i])
			printf(" %s %ld", kinds[i], npk[i]);
		if (i == 5)
			printf("\n     ");
	}
	printf("\nrefused %ld last %04lx  ", kl("guest_nprivbad"), kl("guest_privop"));
	k = kl("aux_lastsys");
	printf("syscalls %ld last %ld err %ld\n", kl("aux_nsys"), k >> 16, k & 0xffff);

	if (lm == 0 && (shmid = shmget(TLOW, 0, 0)) >= 0) {
		lm = (char *)shmat(shmid, (char *)0, SHM_RDONLY);
		if (lm == (char *)-1)
			lm = 0;
	}
	if (lm) {
		s = lm + 0x910;
		printf("Ticks %lu MemErr %d DSErr %d $A9E %04x CPUFlag %d App \"%.*s\"\n",
		    LL(0x16A), (short)LW(0x220), (short)LW(0xAF0), LW(0xA9E),
		    lm[0x12F], s[0] & 31, s + 1);
	} else
		printf("Mac memory: not attached\n");

	/* uinter calls, oldest first, three a line */
	kget("ui_calls", 0L, (char *)c, sizeof c);
	n = kl("ui_ncalls");
	i = n > 12 ? n - 12 : 0;
	for (k = 0; i < n; i++, k++) {
		long *e = &c[2 * (i % NCALL)];

		printf("%c%02lx=%-4ld x%-6ld%s", (int)(e[0] >> 24 & 0xff),
		    e[0] >> 16 & 0xff, (long)(short)e[0], e[1], k % 3 == 2 ? "\n" : "  ");
	}
	if (k % 3)
		printf("\n");
	if (fd >= 0)
		close(fd);
}

main(argc, argv)
	int argc;
	char **argv;
{
	int w = argc > 1 && strcmp(argv[1], "-w") == 0;

	if (geteuid() != 0) {
		fprintf(stderr, "macdiag: root only\n");
		return 1;
	}

	if ((kfd = open("/dev/kmem", O_RDONLY)) < 0) {
		perror("macdiag: /dev/kmem");
		return 1;
	}
	for (;;) {
		show();
		if (!w)
			break;
		fflush(stdout);
		sleep(2);
		printf("\n");
	}
	return 0;
}
