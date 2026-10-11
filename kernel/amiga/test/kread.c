/*
 * kread -- longs of kernel memory through /dev/kmem.
 *
 *	kread address [count]	address in hex
 */
#include <stdio.h>
#include <fcntl.h>

extern long lseek(), strtoul();
extern int read(), open();

int
main(argc, argv)
int argc;
char **argv;
{
	unsigned long a, v;
	int fd, n;

	if (argc < 2 || (fd = open("/dev/kmem", O_RDONLY)) < 0) {
		fprintf(stderr, "usage: kread address [count]\n");
		return 2;
	}
	a = strtoul(argv[1], (char **)0, 16);
	n = argc > 2 ? atoi(argv[2]) : 1;
	if (lseek(fd, (long)a, 0) == -1) {
		perror("kread: lseek");
		return 1;
	}
	while (n-- > 0) {
		if (read(fd, (char *)&v, 4) != 4) {
			perror("kread");
			return 1;
		}
		printf("%08lx: %08lx\n", a, v);
		a += 4;
	}
	return 0;
}
