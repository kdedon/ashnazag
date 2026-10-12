#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int mprotect(), munmap();

#define PG 4096
#define LAST_A (0UL - (1UL << 25))
#define LAST_B (0UL - (1UL << 18))

static void
fail(what)
char *what;
{
	perror(what);
	exit(1);
}

int
main(argc, argv)
int argc;
char **argv;
{
	char path[64];
	char zero[PG];
	char *alias, *high, *seed = 0;
	unsigned long base;
	int fd;

	if (argc != 2 || (strcmp(argv[1], "a") && strcmp(argv[1], "b"))) {
		fprintf(stderr, "usage: %s a|b\n", argv[0]);
		return 2;
	}
	if (sizeof(unsigned long) != 4) {
		fprintf(stderr, "requires a 32-bit native AMIX process\n");
		return 2;
	}
	sprintf(path, "/tmp/hat-boundary-%ld", (long)getpid());
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		fail("open");
	if (unlink(path) < 0)
		fail("unlink");
	memset(zero, 0, sizeof(zero));
	if (write(fd, zero, sizeof(zero)) != sizeof(zero))
		fail("write");
	alias = mmap(0, PG, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_SHARED, fd, 0);
	if (alias == (char *)-1)
		fail("mmap alias");
	alias[0] = 'Q';
	base = LAST_A + PG;
	if (argv[1][0] == 'b') {
		seed = mmap((caddr_t)base, PG, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, fd, 0);
		if (seed != (char *)base)
			fail("mmap seed");
		if (seed[0] != 'Q') {
			fprintf(stderr, "seed alias differs\n");
			return 1;
		}
		base = LAST_B + PG;
	}
	high = mmap((caddr_t)base, PG, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_SHARED | MAP_FIXED, fd, 0);
	if (high != (char *)base)
		fail("mmap high");
	printf("BEGIN %s mprotect untouched %08lx\n", argv[1], base);
	fflush(stdout);
	if (mprotect(high, PG, PROT_READ | PROT_WRITE) < 0)
		fail("mprotect high");
	if (mprotect(alias, PG, PROT_READ | PROT_WRITE) < 0)
		fail("mprotect alias");
	if (high[0] != 'Q') {
		fprintf(stderr, "high alias differs\n");
		return 1;
	}
	high[1] = 'R';
	if (alias[1] != 'R') {
		fprintf(stderr, "shared store differs\n");
		return 1;
	}
	if (mprotect(high, PG, PROT_READ) < 0)
		fail("mprotect resident high");
	if (high[1] != 'R') {
		fprintf(stderr, "protected content differs\n");
		return 1;
	}
	if (munmap(high, PG) < 0 || munmap(alias, PG) < 0)
		fail("munmap");
	if (argv[1][0] == 'b' && munmap(seed, PG) < 0)
		fail("munmap seed");
	close(fd);
	printf("PASS %s\n", argv[1]);
	return 0;
}
