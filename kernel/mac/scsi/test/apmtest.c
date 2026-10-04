/*
 * Host test for apm_scan: print the slice table of a disk image.
 *
 *	apmtest image
 *
 * Output: one line per mapped slice, "s<n> <base> <len> <how> <entry> <name>|<type>",
 * or "nomap" / "error <n>".
 */

#include <stdio.h>
#include "../apm.h"

static FILE *img;

static int
rd(arg, bn, buf)
char *arg;
long bn;
unsigned char *buf;
{
	if (fseek(img, bn * APM_BSIZE, 0) != 0)
		return 5;
	if (fread(buf, 1, APM_BSIZE, img) != APM_BSIZE)
		return 5;
	return 0;
}

static char *hows[] = { "none", "whole", "bzbslice", "bzbrole", "namerole", "unix", "other" };

int
main(argc, argv)
int argc;
char **argv;
{
	struct apm_slice sl[APM_NSLICE];
	int s, r;

	if (argc != 2 || (img = fopen(argv[1], "rb")) == NULL) {
		fprintf(stderr, "usage: apmtest image\n");
		return 2;
	}
	r = apm_scan(rd, (char *)0, sl);
	if (r < 0) {
		printf("nomap\n");
		return 0;
	}
	if (r > 0) {
		printf("error %d\n", r);
		return 0;
	}
	for (s = 0; s < APM_NSLICE; s++)
		if (sl[s].how != APM_NONE)
			printf("s%d %lu %lu %s %d %s|%s\n", s, sl[s].base, sl[s].len,
			    hows[sl[s].how], sl[s].entry, sl[s].name, sl[s].type);
	return 0;
}
