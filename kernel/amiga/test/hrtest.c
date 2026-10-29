/*
 * hrtest MB [forks] -- fill MB megabytes, every long, check them, then
 * fork children that check and rewrite their copy while the parent's
 * stays; then forks fork+exec of /bin/true.  Prints HRTEST PASS or FAIL.
 * A fork with most of memory held fails for want of swap (EAGAIN).
 */
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#define	CH	(1 << 20)

static unsigned long *p[1024];
static int mb;

static unsigned long
pat(i, k, s)
int i;
unsigned long k, s;
{
	return ((unsigned long)i << 20 ^ k << 2 ^ s) * 2654435761UL;
}

static long
check(s, sum)
unsigned long s, *sum;
{
	int i;
	unsigned long k, v;
	long bad = 0;

	*sum = 0;
	for (i = 0; i < mb; i++)
		for (k = 0; k < CH / 4; k++) {
			v = p[i][k];
			*sum += v;
			if (v != pat(i, k, s))
				bad++;
		}
	return bad;
}

int
main(argc, argv)
int argc;
char **argv;
{
	int i, n, st, nf = argc > 2 ? atoi(argv[2]) : 0, ok = 0;
	unsigned long k, sum, sum2;
	long bad;
	struct rlimit rl;
	pid_t c;

	mb = argc > 1 ? atoi(argv[1]) : 64;
	rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_DATA, &rl);
	setrlimit(RLIMIT_VMEM, &rl);
	for (i = 0; i < mb; i++)
		if ((p[i] = (unsigned long *)malloc(CH)) == 0) {
			printf("hrtest: malloc failed at %d MB\n", i);
			mb = i;
			break;
		}
	for (i = 0; i < mb; i++)
		for (k = 0; k < CH / 4; k++)
			p[i][k] = pat(i, k, 0UL);
	bad = check(0UL, &sum);
	printf("hrtest: %d MB filled, sum %lx, %ld bad\n", mb, sum, bad);
	for (n = 0; n < 4; n++) {
		if ((c = fork()) == 0) {
			if (check(0UL, &sum2) || sum2 != sum)
				_exit(1);
			for (i = n; i < mb; i += 16)
				for (k = 0; k < CH / 4; k++)
					p[i][k] = pat(i, k, 0UL) ^ 1;
			_exit(0);
		}
		if (c < 0)
			printf("hrtest: fork: errno %d\n", errno);
		if (c < 0 || waitpid(c, &st, 0) != c || st != 0)
			bad++;
	}
	if (check(0UL, &sum2) || sum2 != sum)
		bad++;
	printf("hrtest: copy on write %s\n", bad ? "FAIL" : "ok");
	for (n = 0; n < nf; n++) {
		if ((c = fork()) == 0) {
			execl("/bin/true", "true", (char *)0);
			_exit(2);
		}
		if (c > 0 && waitpid(c, &st, 0) == c && st == 0)
			ok++;
	}
	if (nf)
		printf("hrtest: fork+exec %d of %d\n", ok, nf);
	printf("HRTEST %s\n", bad || ok != nf ? "FAIL" : "PASS");
	return bad || ok != nf;
}
