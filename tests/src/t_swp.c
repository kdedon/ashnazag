/*
 * t_swp.c -- paging to a SCSI swap area: four processes hold twice the
 * free memory between them, two as one block written with a pattern, two
 * as a list of small blocks grown by many small breaks, and check every
 * word after it comes back, twice.  Needs the disk at SCSI ID 1 (its
 * writes discarded) and a small memory size (GROUP=swap).
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/swap.h>
#include <sys/stat.h>
#include <sys/mkdev.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

#define NPROC	4
#define PASSES	2
#define K	0x9E3779B9UL
#define PAT(id, p, w, n)	((unsigned long)(id) << 26 ^ (unsigned long)(p) << 11 ^ \
				(unsigned long)(w) ^ (unsigned long)(n) * K)

static long pgsz, nw;
static int rp[2], gp[2];

/* report, then wait until every holder has its memory */
static int
ready()
{
	char c = 'r';

	write(rp[1], &c, 1);
	return read(gp[0], &c, 1) == 1;
}

static void
fill(m, id, npg, n)
unsigned long *m;
long npg;
int id, n;
{
	long p, w;

	for (p = 0; p < npg; p++)
		for (w = 0; w < nw; w++)
			m[p * nw + w] = PAT(id, p, w, n);
}

/* bad pages; the first few named, with the pass and page that wrote them */
static long
check(m, id, npg, n)
unsigned long *m;
long npg;
int id, n;
{
	long p, w, bad = 0, badw;
	unsigned long v, d;
	int k;

	for (p = 0; p < npg; p++) {
		for (badw = 0, w = 0; w < nw; w++)
			if (m[p * nw + w] != PAT(id, p, w, n))
				badw++;
		if (badw == 0 || bad++ >= 4)
			continue;
		for (w = 0; m[p * nw + w] == PAT(id, p, w, n); w++)
			;
		v = m[p * nw + w];
		for (k = 0; k <= PASSES; k++)
			if (((d = v ^ w ^ (unsigned long)k * K) & 0x7ff) == 0)
				break;
		t_info("bad", "holder %d pass %d page %ld: %ld bad words from %ld, 0x%lx want 0x%lx (pass %d page %ld)",
		    id, n, p, badw, w, v, PAT(id, p, w, n), k, k <= PASSES ? (long)(d >> 11 & 0x7fff) : -1L);
	}
	return bad;
}

static int
block(id, npg)
int id;
long npg;
{
	unsigned long *m;
	long b;
	int n;

	if ((m = (unsigned long *)malloc(npg * pgsz)) == 0)
		return 10;
	for (n = 0; n < PASSES; n++) {
		fill(m, id, npg, n);
		if (n == 0 && !ready())
			return 11;
		if ((b = check(m, id, npg, n)) != 0) {
			t_info("block", "holder %d pass %d: %ld of %ld pages bad", id, n, b, npg);
			return 20 + n;
		}
	}
	return 0;
}

struct node {
	struct node *next;
	long id, k;
	unsigned long pad[21];
};

static int
list(id, npg)
int id;
long npg;
{
	struct node *h = 0, *q;
	long nn = npg * pgsz / (sizeof (struct node) + 8), k, j;
	int n;

	for (k = 0; k < nn; k++) {
		if ((q = (struct node *)malloc(sizeof *q)) == 0)
			return 10;
		q->next = h;
		q->id = id;
		q->k = k;
		for (j = 0; j < 21; j++)
			q->pad[j] = PAT(id, k, j, 0);
		h = q;
	}
	if (!ready())
		return 11;
	for (n = 0; n < PASSES; n++) {
		for (q = h, k = nn - 1; q; q = q->next, k--) {
			if (q->id != id || q->k != k) {
				t_info("list", "holder %d pass %d node %ld at 0x%lx: id %ld k %ld",
				    id, n, k, (long)q, q->id, q->k);
				return 30 + n;
			}
			for (j = 0; j < 21; j++)
				if (q->pad[j] != PAT(id, k, j, n)) {
					t_info("list", "holder %d pass %d node %ld word %ld: 0x%lx want 0x%lx",
					    id, n, k, j, q->pad[j], PAT(id, k, j, n));
					return 40 + n;
				}
			for (j = 0; j < 21; j++)
				q->pad[j] = PAT(id, k, j, n + 1);
		}
		if (k != -1)
			return 12;
	}
	return 0;
}

int
main()
{
	int i, st, ok = 0;
	pid_t pid[NPROC];
	long fm, npg;
	char c;
	struct rlimit rl;
	swapres_t r;

	t_init("swp", 1500);
	rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_DATA, &rl);
	setrlimit(RLIMIT_VMEM, &rl);
	pgsz = sysconf(_SC_PAGESIZE);
	nw = pgsz / sizeof (long);
	mknod("/tmp/swpdev", S_IFBLK | 0600, makedev(18, 1));
	r.sr_name = "/tmp/swpdev";
	r.sr_start = 64;
	r.sr_length = 120000;
	if (swapctl(SC_ADD, (void *)&r) < 0) {
		t_fail("swapadd", "%s", T_ERR);
		return t_done();
	}
	fm = t_kmem("freemem");
	npg = fm * 2 / NPROC;
	t_info("plan", "freemem %ld, %d holders x %ld pages", fm, NPROC, npg);
	t_check("maxpgio", t_kmem("maxpgio") >= 1024, "%ld", t_kmem("maxpgio"));
	pipe(rp);
	pipe(gp);
	for (i = 0; i < NPROC; i++)
		if ((pid[i] = fork()) == 0)
			_exit(i & 1 ? list(i, npg) : block(i, npg));
	for (i = 0; i < NPROC && read(rp[0], &c, 1) == 1; i++)
		;
	t_info("held", "freemem %ld", t_kmem("freemem"));
	for (i = 0; i < NPROC; i++)
		write(gp[1], "g", 1);
	for (i = 0; i < NPROC; i++) {
		if (t_waitchild(pid[i], &st, 1400) == pid[i] && st == 0)
			ok++;
		else
			t_info("holder", "%d: status 0x%x", i, st);
	}
	t_check("verify", ok == NPROC, "%d of %d holders", ok, NPROC);
	return t_done();
}
