/*
 * memhog MB -- allocate MB megabytes, write a pattern into every page,
 * read it all back; exercises swap.  Prints timings and any mismatch.
 */
#include <sys/types.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int
main(argc, argv)
int argc;
char **argv;
{
	int mb = argc > 1 ? atoi(argv[1]) : 64, i, bad = 0;
	unsigned long n, k, *p[1024];
	struct rlimit rl;
	time_t t0, t1, t2;

	rl.rlim_cur = rl.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_DATA, &rl);
	setrlimit(RLIMIT_VMEM, &rl);
	if (mb > 1024) mb = 1024;
	t0 = time(0);
	for (i = 0; i < mb; i++) {
		if ((p[i] = (unsigned long *)malloc(1 << 20)) == 0) {
			printf("memhog: malloc failed at %d MB\n", i);
			mb = i;
			break;
		}
		for (k = 0; k < (1 << 20) / 4; k += 1024)	/* one long per 4 KB page */
			p[i][k] = (unsigned long)i << 20 ^ k ^ 0x5a5a0000UL;
	}
	t1 = time(0);
	for (i = 0; i < mb; i++)
		for (k = 0; k < (1 << 20) / 4; k += 1024)
			if (p[i][k] != ((unsigned long)i << 20 ^ k ^ 0x5a5a0000UL))
				bad++;
	t2 = time(0);
	printf("memhog: %d MB written in %ld s, verified in %ld s, %d bad pages\n",
	    mb, (long)(t1 - t0), (long)(t2 - t1), bad);
	return bad != 0;
}
