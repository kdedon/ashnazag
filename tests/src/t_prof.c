/*
 * t_prof.c -- profil(2): profiled CPU-bound children survive with intact
 * registers and FP state while competing for the CPU, and the histogram
 * fills.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "t.h"

#define NBUF	4096
#define SECS	8

extern void _start();

static unsigned short buf[NBUF];

/* one unit of FP and integer work; same inputs give the same results */
static int
work(seed, fr)
	long seed;
	double *fr;
{
	volatile double x = 1.0 + seed * 1e-3, y = 0.5;
	long a = seed, b = 0x9e3779b9;
	int i;

	for (i = 0; i < 2000; i++) {
		x = x * 1.0000001 + y / (x + 3.0);
		y = y * 0.9999 + 1e-4;
		a = a * 69069 + 1 + (b >> 3);
		b ^= a << 5;
	}
	*fr = x + y;
	return (int)(a ^ b);
}

/* exits 0 if every round matched round 0, 3 on a mismatch */
static void
child(prof)
	int prof;
{
	double f0, f;
	int r0, r;
	long t0;
	unsigned long sum, i;

	if (prof)
		profil(buf, sizeof(buf), (unsigned int)_start, 0x8000);
	r0 = work(7, &f0);
	t0 = t_now_ms();
	while (t_now_ms() - t0 < SECS * 1000) {
		r = work(7, &f);
		if (r != r0 || f != f0)
			_exit(3);
	}
	if (prof) {
		profil((unsigned short *)0, 0, 0, 0);
		for (sum = i = 0; i < NBUF; i++)
			sum += buf[i];
		_exit(sum ? 0 : 4);
	}
	_exit(0);
}

int
main()
{
	pid_t pid[3];
	int i, st, ok;

	t_init("prof", SECS * 4 + 30);
	for (i = 0; i < 3; i++) {
		pid[i] = fork();
		if (pid[i] == 0)
			child(i < 2);
	}
	for (i = 0; i < 3; i++) {
		ok = t_waitchild(pid[i], &st, SECS * 3 + 20) == pid[i] && WIFEXITED(st) &&
		    WEXITSTATUS(st) == 0;
		t_check(i < 2 ? "profiled_child" : "plain_child", ok,
		    "status 0x%x (signal %d, exit %d; 3 = bad result, 4 = empty histogram)",
		    st, WIFSIGNALED(st) ? WTERMSIG(st) : 0, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	}
	return t_done();
}
