/*
 * sampler.c -- user PC samples for any program linked with it.
 *
 * With QPROF=file in the environment a user-time interval timer takes the
 * PC at each clock tick, in the program and in its shared objects.  On
 * exit or SIGTERM file gets the words 'PROF', samples, objects, user,
 * system and elapsed ticks; per loaded object its address and a 60-byte
 * name (the program first, named ""); then the PCs.
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <siginfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/ucontext.h>

#define MAXS (1 << 18)		/* 43 minutes at 100 ticks a second */

extern Elf32_Dyn _DYNAMIC[];
static unsigned long *pcs, ns;
static char *path;
static pid_t pid;
static clock_t t0;

void qprof_start(void);
void _exithandle(void);

static void
tick(int sig, siginfo_t *si, ucontext_t *uc)
{
	if (ns < MAXS)
		pcs[ns++] = uc->uc_mcontext.gregs[R_PC];
}

static struct link_map *
objects(void)
{
	Elf32_Dyn *d;

	for (d = _DYNAMIC; d->d_tag != DT_NULL; d++)
		if (d->d_tag == DT_DEBUG && d->d_un.d_ptr)
			return ((struct r_debug *)d->d_un.d_ptr)->r_map;
	return NULL;
}

static void
trailer(void)
{
	static struct itimerval off;
	unsigned long h[6], o[16];
	struct link_map *m;
	struct tms t;
	clock_t now;
	int fd;

	/* a vfork child's exit runs this in our memory */
	if (path == NULL || getpid() != pid)
		return;
	setitimer(ITIMER_VIRTUAL, &off, NULL);
	now = times(&t);
	/* opened late: programs may close or reuse descriptors they did not open */
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		return;
	h[0] = 0x50524f46;
	h[1] = ns;
	h[2] = 0;
	h[3] = t.tms_utime;
	h[4] = t.tms_stime;
	h[5] = now - t0;
	for (m = objects(); m != NULL; m = m->l_next)
		h[2]++;
	write(fd, h, sizeof h);
	for (m = objects(); m != NULL; m = m->l_next) {
		memset(o, 0, sizeof o);
		o[0] = m->l_addr;
		if (m->l_prev != NULL && m->l_name != NULL)
			strncpy((char *)&o[1], m->l_name, sizeof o - 5);
		write(fd, o, sizeof o);
	}
	write(fd, pcs, ns * sizeof pcs[0]);
	close(fd);
	path = NULL;
}

/*
 * libc's exit pops the atexit table shared with a vfork child, so a
 * child's exit leaves the parent none: write the samples here instead.
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
	struct sigaction sa;
	struct itimerval it;
	struct tms t;
	char *f = getenv("QPROF");

	if (f == NULL || (pcs = malloc(MAXS * sizeof pcs[0])) == NULL)
		return;
	path = f;
	t0 = times(&t);
	pid = getpid();
	signal(SIGTERM, term);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = (void (*)())tick;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGVTALRM, &sa, NULL);
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 1000000 / sysconf(_SC_CLK_TCK);
	it.it_value = it.it_interval;
	setitimer(ITIMER_VIRTUAL, &it, NULL);
}

/* run qprof_start() before main */
asm(".section .init\n\tjsr qprof_start\n\t.previous");
