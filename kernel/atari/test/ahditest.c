/* Prints the slices the kernel's AHDI scan finds in a disk image. */
#include <stdio.h>
#include <errno.h>
#include "../ahdi.h"

static int
rd(char *arg, long bn, unsigned char *buf)
{
	FILE *f = (FILE *)arg;

	if (fseek(f, bn * AHDI_BSIZE, SEEK_SET) != 0 ||
	    fread(buf, AHDI_BSIZE, 1, f) != 1)
		return EIO;
	return 0;
}

int
main(int argc, char **argv)
{
	struct ahdi_slice sl[AHDI_NSLICE];
	FILE *f;
	int s, r;

	if (argc != 2 || (f = fopen(argv[1], "rb")) == NULL) {
		fprintf(stderr, "usage: ahditest image\n");
		return 2;
	}
	if ((r = ahdi_scan(rd, (char *)f, sl)) != 0) {
		printf("no AHDI table (%d)\n", r);
		return 1;
	}
	for (s = 0; s < AHDI_NSLICE; s++)
		if (sl[s].len)
			printf("s%d %lu %lu %s\n", s, sl[s].base, sl[s].len, sl[s].id);
	return 0;
}
