/*
 * tflush.h -- the kernel's flush counters: whole cache and ATC flushes
 * must be rare next to page and line ones.  t_flushes(f, 0) reads them
 * into f[5]; t_flushes(f, what) checks the run since.  Needs getksym.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static char *t_fln[5] = { "hat_flall_n", "hat_flpg_n", "hat_fpush_n", "guest_cflush_all", "guest_cflush_scoped" };

/* a kernel or module long, -1 if unknown */
static long
t_ksym(char *name)
{
	extern int getksym();
	unsigned long a = 0, i;
	long v = -1;
	int fd;

	if (getksym(name, &a, &i) < 0 || (fd = open("/dev/kmem", O_RDONLY)) < 0)
		return -1;
	if (lseek(fd, (off_t)a, 0) == -1 || read(fd, (char *)&v, 4) != 4)
		v = -1;
	close(fd);
	return v;
}

static void
t_flushes(long *f, char *what)
{
	long d[5];
	int i;

	/* the guest counters appear when its module loads */
	for (i = 0; i < 5; i++)
		if ((d[i] = t_ksym(t_fln[i])) == -1 && i >= 3)
			d[i] = 0;
	if (what == 0) {
		memcpy(f, d, sizeof d);
		return;
	}
	for (i = 0; i < 5; i++)
		if (f[i] == -1 || (d[i] -= f[i]) < 0)
			return;
	t_info("flushes", "%s: whole %ld, page %ld, frame pushes %ld; guest whole %ld, scoped %ld",
	    what, d[0], d[1], d[2], d[3], d[4]);
	t_check("flush_scoped", d[1] + d[4] > 10 * (d[0] + d[3]), "%ld whole flushes, %ld scoped",
	    d[0] + d[3], d[1] + d[4]);
}

