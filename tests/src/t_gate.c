/*
 * t_gate.c -- cost of the guest vector gates on native system calls.
 *
 * trap #0 and trap #10 reach the same system call path; on a kernel
 * with guest support trap #0 passes its gate, trap #10 has none.  Both
 * run getpid in alternating rounds in several fresh processes; the best
 * round of each over all processes gives the time per call.  The
 * emulator's cost for a call varies by up to 50% with where a process's
 * code lands, and a process keeps its placement, so one process cannot
 * give a stable ratio.  Pass: both return the pid, and trap #0 costs at
 * most a quarter more than trap #10 (the gate is a few instructions; a
 * real regression is slower in every process).  A round is sized to run
 * ROUNDMS on this host, so a loaded or slow host takes as long as an idle
 * one.
 */
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include "t.h"

#define	CALLS	500000	/* at most per round: about 50 clock ticks, one tick is 2% */
#define	ROUNDMS	500
#define	MINCALLS 5000
#define	ROUNDS	3
#define	PROCS	5

static int
trap0()
{
	register int r __asm__("%d0");

	__asm__ __volatile__("moveq &20,%%d0\n\ttrap &0" : "=d" (r) : : "d1", "a0", "a1");
	return r;
}

static int
trap10()
{
	register int r __asm__("%d0");

	__asm__ __volatile__("moveq &20,%%d0\n\ttrap &10" : "=d" (r) : : "d1", "a0", "a1");
	return r;
}

static long calls = CALLS;

/* calls per round: what takes ROUNDMS, by a short trial of trap #10 */
static void
size()
{
	long t0, t1;
	int i, n = MINCALLS;

	for (;;) {
		t0 = t_now_ms();
		for (i = 0; i < n; i++)
			trap10();
		t1 = t_now_ms();
		if (t1 - t0 >= 50 || n >= CALLS)
			break;
		n *= 2;
	}
	calls = t1 > t0 ? (long)n * ROUNDMS / (t1 - t0) : CALLS;
	if (calls > CALLS)
		calls = CALLS;
	if (calls < MINCALLS)
		calls = MINCALLS;
}

/* microseconds per call x 100 */
static long
timeit(f)
int (*f)();
{
	long t0, t1;
	int i;

	t0 = t_now_ms();
	for (i = 0; i < calls; i++)
		(*f)();
	t1 = t_now_ms();
	return (t1 - t0) * 100000L / calls;
}

/* best of ROUNDS alternating rounds, sent to fd as b0, b10 */
static void
child(fd)
int fd;
{
	long b[2], t;
	int k;

	size();
	for (k = 0; k < ROUNDS; k++) {
		t = timeit(trap0);
		if (k == 0 || t < b[0])
			b[0] = t;
		t = timeit(trap10);
		if (k == 0 || t < b[1])
			b[1] = t;
	}
	(void)write(fd, (char *)b, sizeof b);
	_exit(0);
}

int
main()
{
	long b0 = 0, b10 = 0, b[2];
	int k, n = 0, pid = getpid(), fds[2];

	t_init("gate", 120);
	t_info("gates", "%s", t_kmem("guest_loading") == -1 ? "absent" : "present");
	t_check("trap0_getpid", trap0() == pid, "trap #0 getpid gave %d, pid %d", trap0(), pid);
	t_check("trap10_getpid", trap10() == pid, "trap #10 getpid gave %d, pid %d", trap10(), pid);
	if (pipe(fds) < 0) {
		t_check("pipe", 0, "pipe failed");
		return t_done();
	}
	for (k = 0; k < PROCS; k++) {
		switch (fork()) {
		case -1:
			break;
		case 0:
			child(fds[1]);
		default:
			if (read(fds[0], (char *)b, sizeof b) == sizeof b) {
				if (n == 0 || b[0] < b0)
					b0 = b[0];
				if (n == 0 || b[1] < b10)
					b10 = b[1];
				n++;
			}
			(void)wait((int *)0);
		}
		t_rearm(120);
	}
	t_check("procs", n == PROCS, "%d of %d timing processes reported", n, PROCS);
	t_info("trap0_us", "%ld.%02ld", b0 / 100, b0 % 100);
	t_info("trap10_us", "%ld.%02ld", b10 / 100, b10 % 100);
	t_info("gate_cost", "%ld/100 us per call (%ld%%)", b0 - b10,
	    b10 ? (b0 - b10) * 100 / b10 : 0L);
	t_check("overhead", n > 0 && b0 <= b10 + b10 / 4, "trap #0 %ld vs trap #10 %ld (1/100 us)", b0, b10);
	return t_done();
}
