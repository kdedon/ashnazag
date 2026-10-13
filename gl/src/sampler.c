/*
 * sampler.c -- PC histogram for any program linked with it.
 *
 * With QPROF=file in the environment the kernel counts the user PC at each
 * clock tick (profil), one 16-bit counter per 4 bytes of the program's text;
 * ticks in shared libraries are not counted.  On exit or SIGTERM file gets
 * the words 'PROF', text start, counters, user, system and elapsed ticks,
 * then the counters.
 */
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/times.h>

extern char etext[];
static unsigned short *hist;
static unsigned long lo, nh;
static char *path;
static pid_t pid;
static clock_t t0;

void qprof_start(void);
void _exithandle(void);

static void
trailer(void)
{
	unsigned long h[6];
	struct tms t;
	clock_t now;
	int fd;

	/* a vfork child's exit runs this in our memory */
	if (path == NULL || getpid() != pid)
		return;
	now = times(&t);
	profil(hist, 0, 0, 0);
	/* opened late: programs may close or reuse descriptors they did not open */
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		return;
	h[0] = 0x50524f46;
	h[1] = lo;
	h[2] = nh;
	h[3] = t.tms_utime;
	h[4] = t.tms_stime;
	h[5] = now - t0;
	write(fd, h, sizeof h);
	write(fd, hist, nh * sizeof hist[0]);
	close(fd);
	path = NULL;
}

/*
 * libc's exit pops the atexit table shared with a vfork child, so a
 * child's exit leaves the parent none: write the counters here instead.
 */
void
exit(int status)
{
	trailer();
	_exithandle();
	_exit(status);
}

static void
term(int sig)
{
	trailer();
	_exit(1);
}

void
qprof_start(void)
{
	struct tms t;
	char *f = getenv("QPROF");

	lo = (unsigned long)qprof_start & 0xff000000;
	nh = ((unsigned long)etext - lo) / 4 + 1;
	if (f == NULL || (hist = calloc(nh, sizeof hist[0])) == NULL)
		return;
	path = f;
	t0 = times(&t);
	pid = getpid();
	signal(SIGTERM, term);
	/* scale 0x8000: byte offset (pc - lo) / 2, a counter per 4 bytes */
	profil(hist, nh * sizeof hist[0], lo, 0x8000);
}

/* run qprof_start() before main */
asm(".section .init\n\tjsr qprof_start\n\t.previous");
