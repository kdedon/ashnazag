/*
 * tosstat -- the TOS environment's counters.
 *
 *	tosstat [seconds]
 *
 * With seconds: what changed over that time, and per-second rates of
 * trap #13 and privileged instructions.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "tosio.h"

static void
get(fd, s)
	int fd;
	struct tosstat *s;
{
	if (ioctl(fd, TOSIOC_STAT, s) < 0) {
		perror("tosstat: TOSIOC_STAT");
		exit(1);
	}
}

main(argc, argv)
	int argc;
	char **argv;
{
	struct tosstat a, b;
	int fd, n = argc > 1 ? atoi(argv[1]) : 0, i;

	if ((fd = open("/dev/tos", O_RDONLY)) < 0) {
		perror("tosstat: /dev/tos");
		return 1;
	}
	memset(&a, 0, sizeof a);
	if (n > 0) {
		get(fd, &a);
		sleep(n);
	}
	get(fd, &b);
	printf("tosstat: absent %lu top %lu berr %lu buserr %lu iow %lu mmu %lu illegal %lu\n",
	    b.ts_absent - a.ts_absent, b.ts_top - a.ts_top, b.ts_berr - a.ts_berr,
	    b.ts_refl[2] - a.ts_refl[2], b.ts_iow - a.ts_iow, b.ts_mmu - a.ts_mmu,
	    b.ts_refl[4] - a.ts_refl[4]);
	printf("tosstat: priv %lu fpriv %lu ftrap %lu sys %lu stop %lu vbl %lu isp %lu tcheld %lu refl",
	    b.ts_priv - a.ts_priv, b.ts_fpriv - a.ts_fpriv, b.ts_ftrap - a.ts_ftrap,
	    b.ts_sys - a.ts_sys, b.ts_stop - a.ts_stop, b.ts_vbl - a.ts_vbl,
	    b.ts_isp - a.ts_isp, b.ts_tcheld - a.ts_tcheld);
	for (i = 0; i < 64; i++)
		if (b.ts_refl[i] != a.ts_refl[i])
			printf(" %d:%lu", i, b.ts_refl[i] - a.ts_refl[i]);
	printf("\n");
	if (n > 0)
		printf("tosstat: %d s: trap13 %lu/s priv %lu/s fpriv %lu/s ftrap %lu/s\n", n,
		    (b.ts_refl[45] - a.ts_refl[45]) / n, (b.ts_priv - a.ts_priv) / n,
		    (b.ts_fpriv - a.ts_fpriv) / n, (b.ts_ftrap - a.ts_ftrap) / n);
	return 0;
}
