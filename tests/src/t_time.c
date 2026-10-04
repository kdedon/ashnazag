/*
 * t_time.c -- time(), gettimeofday(), times() against sleep().
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

int
main()
{
	struct tms t0, t1;
	struct timeval tv, prev;
	clock_t c0, c1;
	time_t s0, s1;
	long m0, m1, hz, lb0, lb1, dt;
	int i, back = 0, badusec = 0;
	volatile long spin;

	t_init("time", 50);
	hz = sysconf(_SC_CLK_TCK);
	t_info("clk_tck", "%ld", hz);
	s0 = time((time_t *)0);
	t_info("date", "time() %ld, %s", (long)s0, ctime(&s0));
	t_check("time_plausible", s0 > 0, "time() %ld", (long)s0);

	lb0 = t_kmem("lbolt");
	c0 = times(&t0);
	m0 = t_now_ms();
	s0 = time((time_t *)0);
	sleep(2);
	s1 = time((time_t *)0);
	m1 = t_now_ms();
	c1 = times(&t1);
	lb1 = t_kmem("lbolt");
	/* sleep() is alarm(), which counts whole seconds: sleep(2) takes 1 to 2 s */
	t_check("time_sleep2", s1 - s0 >= 1 && s1 - s0 <= 3, "time() advanced %ld s", (long)(s1 - s0));
	t_check("gettimeofday_sleep2", m1 - m0 >= 900 && m1 - m0 <= 2600, "advanced %ld ms", m1 - m0);
	dt = (long)(c1 - c0) * 1000 / (hz > 0 ? hz : 60);
	t_check("times_sleep2", dt >= 900 && dt <= 2600 && dt * 100 >= (m1 - m0) * 95 &&
	    dt * 100 <= (m1 - m0) * 105, "times() advanced %ld ticks = %ld ms at %ld Hz, gettimeofday %ld ms",
	    (long)(c1 - c0), dt, hz, m1 - m0);
	if (lb0 != -1 && lb1 != -1)
		t_info("lbolt", "advanced %ld over %ld ms", lb1 - lb0, m1 - m0);
	t_info("clock", "ticks_til_clock %ld", t_kmem("ticks_til_clock"));

	/*
	 * 5 s against a busy poll loop: tick rate within 5 %.  The marks let
	 * the host compare with its own clock (the runner's serial.ts).
	 */
	c0 = times(&t0);
	m0 = t_now_ms();
	s0 = time((time_t *)0);
	lb0 = t_kmem("mac_ticks");
	t_info("wall_mark0", "%ld ms", m0);
	poll((struct pollfd *)0, 0, 5000);
	m1 = t_now_ms();
	t_info("wall_mark1", "%ld ms", m1);
	lb1 = t_kmem("mac_ticks");
	c1 = times(&t1);
	s1 = time((time_t *)0);
	if (lb0 != -1 && lb1 != -1)
		t_info("mac_ticks", "VIA1 ticks %ld over %ld ms", lb1 - lb0, m1 - m0);
	dt = (long)(c1 - c0) * 1000 / (hz > 0 ? hz : 60);
	t_check("poll5_gettimeofday", m1 - m0 >= 4900 && m1 - m0 <= 5500, "%ld ms", m1 - m0);
	t_check("poll5_times_vs_tod", dt * 100 >= (m1 - m0) * 95 && dt * 100 <= (m1 - m0) * 105,
	    "times %ld ms vs gettimeofday %ld ms", dt, m1 - m0);
	t_check("poll5_time", s1 - s0 >= 4 && s1 - s0 <= 6, "time() %ld s", (long)(s1 - s0));

	/* monotonic, microseconds in range */
	gettimeofday(&prev, (void *)0);
	for (i = 0; i < 20000; i++) {
		gettimeofday(&tv, (void *)0);
		if (tv.tv_usec < 0 || tv.tv_usec >= 1000000)
			badusec++;
		if (tv.tv_sec < prev.tv_sec || (tv.tv_sec == prev.tv_sec && tv.tv_usec < prev.tv_usec))
			back++;
		prev = tv;
	}
	t_check("gettimeofday_monotonic", back == 0 && badusec == 0, "%d steps back, %d bad usec", back, badusec);
	t_info("gettimeofday_resolution", "last value %ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);

	/* user CPU time accrues while spinning */
	times(&t0);
	m0 = t_now_ms();
	spin = 0;
	while (t_now_ms() - m0 < 1500)
		for (i = 0; i < 1000; i++)
			spin += i;
	times(&t1);
	dt = (long)(t1.tms_utime + t1.tms_stime - t0.tms_utime - t0.tms_stime) * 1000 / (hz > 0 ? hz : 60);
	t_check("times_cpu", dt >= 700 && dt <= 2000, "user+sys %ld ms for 1500 ms spin (user %ld sys %ld ticks)",
	    dt, (long)(t1.tms_utime - t0.tms_utime), (long)(t1.tms_stime - t0.tms_stime));
	return t_done();
}
