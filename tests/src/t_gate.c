/*
 * t_gate.c -- cost of the guest vector gates on native system calls.
 *
 * trap #0 and trap #10 reach the same system call path; on a kernel
 * with guest support trap #0 passes its gate, trap #10 has none.  Both
 * run getpid in alternating rounds; the best round of each gives the
 * time per call.  Pass: both return the pid, and trap #0 costs at most
 * a quarter more than trap #10 (the gate is a few instructions; the
 * bound only catches a gross regression under emulator noise).
 */
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include "t.h"

#define	CALLS	1000000	/* about 100 clock ticks per round: one tick is 1% */
#define	ROUNDS	5

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

/* microseconds per call x 100 */
static long
timeit(f)
int (*f)();
{
	long t0, t1;
	int i;

	t0 = t_now_ms();
	for (i = 0; i < CALLS; i++)
		(*f)();
	t1 = t_now_ms();
	return (t1 - t0) * 100000L / CALLS;
}

int
main()
{
	long b0 = 0, b10 = 0, t;
	int k, pid = getpid();

	t_init("gate", 120);
	t_info("gates", "%s", t_kmem("guest_loading") == -1 ? "absent" : "present");
	t_check("trap0_getpid", trap0() == pid, "trap #0 getpid gave %d, pid %d", trap0(), pid);
	t_check("trap10_getpid", trap10() == pid, "trap #10 getpid gave %d, pid %d", trap10(), pid);
	for (k = 0; k < ROUNDS; k++) {
		t = timeit(trap0);
		if (k == 0 || t < b0)
			b0 = t;
		t = timeit(trap10);
		if (k == 0 || t < b10)
			b10 = t;
		t_rearm(120);
	}
	t_info("trap0_us", "%ld.%02ld", b0 / 100, b0 % 100);
	t_info("trap10_us", "%ld.%02ld", b10 / 100, b10 % 100);
	t_info("gate_cost", "%ld/100 us per call (%ld%%)", b0 - b10,
	    b10 ? (b0 - b10) * 100 / b10 : 0L);
	t_check("overhead", b0 <= b10 + b10 / 4, "trap #0 %ld vs trap #10 %ld (1/100 us)", b0, b10);
	return t_done();
}
