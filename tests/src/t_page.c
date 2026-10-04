/*
 * t_page.c -- paging: several processes touch more anonymous memory than
 * is free, then check every page after it comes back, with fork (COW) and
 * exit churn.  Swap free space, from swapctl, must drop while the memory
 * is held and come back once the processes are gone.
 *
 * The amount is sized from free memory and from what swap can still
 * reserve; when swap is too small to exceed free memory the same checks
 * run without paging and page.paged_out is skipped.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/swap.h>
#include <sys/signal.h>
#include <sys/fault.h>
#include <sys/syscall.h>
#include <sys/procfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define NPROC	4		/* holders at a time */
#define ROUNDS	2		/* holder generations: exit churn */
#define NSWAP	8

static long pgsz;

/* word w of page p of holder id, written in pass n */
#define PAT(id, p, w, n)	((unsigned long)(id) << 26 ^ (unsigned long)(p) << 11 ^ \
				(unsigned long)(w) ^ (unsigned long)(n) * 0x9E3779B9)

/* swap pages in total and free (in kernel pages), -1 when unknown */
static int
swapfree(total, freep)
long *total, *freep;
{
	static char names[NSWAP][80];
	struct {
		int n;
		struct swapent e[NSWAP];
	} t;
	int i, n;

	*total = *freep = -1;
	n = swapctl(SC_GETNSWP, (void *)0);
	if (n <= 0 || n > NSWAP)
		return -1;
	t.n = n;
	for (i = 0; i < n; i++)
		t.e[i].ste_path = names[i];
	if (swapctl(SC_LIST, (void *)&t) < 0)
		return -1;
	*total = *freep = 0;
	for (i = 0; i < t.n; i++) {
		*total += t.e[i].ste_pages;
		*freep += t.e[i].ste_free;
	}
	return t.n;
}

static int
check(m, id, p0, p1, n)
unsigned long *m;
int id, n;
long p0, p1;
{
	long p, w, nw = pgsz / sizeof (long);

	for (p = p0; p < p1; p++)
		for (w = 0; w < nw; w += 7)
			if (m[p * nw + w] != PAT(id, p, w, n))
				return 1;
	return 0;
}

static void
fill(m, id, p0, p1, n)
unsigned long *m;
int id, n;
long p0, p1;
{
	long p, w, nw = pgsz / sizeof (long);

	for (p = p0; p < p1; p++)
		for (w = 0; w < nw; w++)
			m[p * nw + w] = PAT(id, p, w, n);
}

/*
 * One holder: touch npg pages, report on rfd, wait for go on gfd, then
 * verify forwards and backwards, rewrite, fork a child that checks and
 * writes the shared pages (copy on write), and verify again.  A fork
 * reserves swap for the whole copy, so holders fork one at a time: each
 * takes a token from tfd and reports on rfd when its child is done.
 */
static int
holder(id, npg, rfd, gfd, tfd)
int id, rfd, gfd, tfd;
long npg;
{
	unsigned long *m;
	pid_t pid;
	long p;
	int st, r;
	char c = 'r';

	/* report even on failure: the parent counts one byte per holder */
	m = (unsigned long *)malloc(npg * pgsz);
	if (m != 0)
		fill(m, id, 0L, npg, 0);
	write(rfd, &c, 1);
	if (m == 0)
		return 10;
	if (read(gfd, &c, 1) != 1)
		return 11;
	if (check(m, id, 0L, npg, 0))
		return 12;
	for (p = npg - 1; p >= 0; p--)
		if (check(m, id, p, p + 1, 0))
			return 13;
	fill(m, id, 0L, npg, 1);
	if (read(tfd, &c, 1) != 1)
		return 17;
	pid = fork();
	if (pid == -1) {
		write(rfd, &c, 1);
		return 14;
	}
	if (pid == 0) {
		if (check(m, id, 0L, npg, 1))
			_exit(1);
		fill(m, id, 0L, npg / 2, 2);
		_exit(check(m, id, 0L, npg / 2, 2) || check(m, id, npg / 2, npg, 1) ? 2 : 0);
	}
	r = t_waitchild(pid, &st, 600) != pid || st != 0;
	write(rfd, &c, 1);
	if (r)
		return 15;
	if (check(m, id, 0L, npg, 1))
		return 16;
	free(m);
	return 0;
}

static void
onalrm(sig)
int sig;
{
}

/* resident pages of process pid, -1 when unknown; *wchan where it sleeps */
static long
rss(pid, wchan)
pid_t pid;
long *wchan;
{
	char path[32];
	prpsinfo_t ps;
	int fd;
	long r = -1;

	sprintf(path, "/proc/%05ld", (long)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (ioctl(fd, PIOCPSINFO, &ps) == 0) {
		r = ps.pr_rssize;
		*wchan = (long)ps.pr_wchan;
	}
	close(fd);
	return r;
}

int
main()
{
	pid_t pid[NPROC];
	long fm, avail, npg, sw0, sf0, swt, sf, sf1, r, maxdrop = 0;
	long rsum = 0, held = 0, wchan = 0;
	int rp[2], gp[2], tp[2], i, n, round, st, bad = 0, first = 0, pressure;
	char c;

	t_init("page", 900);
	/* dead holders show up as write errors */
	signal(SIGPIPE, SIG_IGN);
	pgsz = sysconf(_SC_PAGESIZE);
	if (swapfree(&sw0, &sf0) <= 0) {
		t_fail("swapctl", "SC_LIST: %s", T_ERR);
		return t_done();
	}
	fm = t_kmem("freemem");
	/* reservable anonymous pages: ani_max - ani_resv */
	avail = t_kmem("anoninfo");
	avail = avail == -1 ? sf0 : avail - t_kmem("anoninfo+8");
	if (avail > sf0)
		avail = sf0;
	t_info("start", "swap %ld pages, %ld free; freemem %ld; reservable %ld",
	    sw0, sf0, fm, avail);
	/* twice free memory; with one fork's copy, within 3/4 of what swap can reserve */
	npg = fm > 0 ? fm * 2 : avail;
	if (npg > avail * 3 / 4 * NPROC / (NPROC + 1))
		npg = avail * 3 / 4 * NPROC / (NPROC + 1);
	/* even with all of freemem resident, under 3/4 of the touched pages */
	pressure = fm > 0 && npg > fm + fm / 2;
	npg /= NPROC;
	t_info("plan", "%d rounds of %d holders x %ld pages (%ld KB each)%s",
	    ROUNDS, NPROC, npg, npg * pgsz / 1024, pressure ? "" : ", no paging");

	for (round = 0; round < ROUNDS; round++) {
		if (pipe(rp) == -1 || pipe(gp) == -1 || pipe(tp) == -1) {
			t_fail("pipe", "%s", T_ERR);
			return t_done();
		}
		for (i = 0; i < NPROC; i++) {
			pid[i] = fork();
			if (pid[i] == 0) {
				close(rp[0]);
				close(gp[1]);
				close(tp[1]);
				_exit(holder(round * NPROC + i, npg, rp[1], gp[0], tp[0]));
			}
			if (pid[i] == -1) {
				t_fail("fork", "%s", T_ERR);
				close(gp[1]);
				close(tp[1]);
				while (--i >= 0)
					t_waitchild(pid[i], &st, 300);
				return t_done();
			}
		}
		close(rp[1]);
		close(gp[0]);
		close(tp[0]);
		/* a holder that cannot get memory within 300 s has stalled */
		signal(SIGALRM, onalrm);
		alarm(300);
		for (n = 0; n < NPROC && read(rp[0], &c, 1) == 1; n++)
			;
		t_rearm(900);
		if (n < NPROC) {
			for (i = 0; i < NPROC; i++) {
				rss(pid[i], &wchan);
				t_info("stalled", "holder %d pid %ld sleeps on 0x%lx", i,
				    (long)pid[i], wchan);
			}
			for (i = 0; i < NPROC; i++) {
				kill(pid[i], SIGKILL);
				waitpid(pid[i], &st, 0);
			}
			t_fail("touch", "%d of %d holders done touching in 300 s; freemem %ld",
			    n, NPROC, t_kmem("freemem"));
			return t_done();
		}
		/* everything is touched: swap slots held, part paged out */
		swapfree(&swt, &sf);
		if (sf0 - sf > maxdrop)
			maxdrop = sf0 - sf;
		for (i = 0; i < NPROC; i++)
			if ((r = rss(pid[i], &wchan)) >= 0) {
				rsum += r;
				held += npg;
			}
		for (i = 0; i < NPROC; i++)
			write(gp[1], "g", 1);
		close(gp[1]);
		/* only this process can write tokens: holders see EOF if it dies */
		for (i = 0; i < NPROC; i++)
			if (write(tp[1], "t", 1) != 1 || read(rp[0], &c, 1) != 1)
				break;
		close(tp[1]);
		close(rp[0]);
		for (i = 0; i < NPROC; i++) {
			if (t_waitchild(pid[i], &st, 800) != pid[i])
				st = 255 << 8;
			if (st != 0 && bad++ == 0)
				first = WIFSIGNALED(st) ? 200 + WTERMSIG(st) : WEXITSTATUS(st);
		}
	}
	t_check("verify", bad == 0, "%d of %d holders failed, first code %d",
	    bad, ROUNDS * NPROC, first);
	t_check("swap_drop", maxdrop >= npg * NPROC * 9 / 10,
	    "swap free dropped %ld pages, %ld touched", maxdrop, npg * NPROC);
	if (!pressure)
		t_skip("paged_out", "swap (%ld reservable) too small to exceed freemem %ld",
		    avail, fm);
	else
		t_check("paged_out", held > 0 && rsum < held - held / 4,
		    "resident %ld of %ld touched pages", rsum, held);
	t_info("resident", "%ld of %ld touched pages resident; swap drop %ld pages",
	    rsum, held, maxdrop);
	sleep(1);
	swapfree(&swt, &sf1);
	t_check("swap_recover", swt == sw0 && sf0 - sf1 < 16,
	    "swap %ld -> %ld pages, free %ld -> %ld", sw0, swt, sf0, sf1);
	return t_done();
}
