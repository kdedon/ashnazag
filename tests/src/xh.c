/*
 * xh.c -- exec target, built with PADSZ bytes of initialized data so the
 * file ends at chosen offsets within its last 4 KB page.  Checks its data
 * and bss, then acts on argv[1]:
 *	(none)		exit 0 if the image is intact
 *	exit N		exit N
 *	args A B	exit 0 if argv[2..3] == A B
 *	env NAME VAL	exit 0 if getenv(NAME) == VAL
 *	fdclosed N	exit 0 if fd N is not open
 *	fdopen N	exit 0 if fd N is open
 * Exit 2: initialized data wrong; 3: bss not zero.
 */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "xhpad.h"	/* generated: PADKIND, PADSZ, pad[] */

static char bss[20000];
static int dwords[4] = { 0x11223344, 0x55667788, 0x0badcafe, 0x7fffffff };

int
main(argc, argv)
int argc;
char **argv;
{
	long i;
	char *e;

	for (i = 0; i < PADSZ; i++)
		if (pad[i] != (char)('A' + (i * 7 + i / 26) % 26))
			return 2;
	if (dwords[2] != 0x0badcafe || dwords[3] != 0x7fffffff)
		return 2;
	for (i = 0; i < (long)sizeof bss; i++)
		if (bss[i] != 0)
			return 3;
	memset(bss, 0x5a, sizeof bss);
	if (argc < 2)
		return 0;
	if (strcmp(argv[1], "exit") == 0 && argc > 2)
		return atoi(argv[2]);
	if (strcmp(argv[1], "args") == 0 && argc > 3)
		return !(strcmp(argv[2], "hello world") == 0 && strcmp(argv[3], "") == 0 && argc == 4);
	if (strcmp(argv[1], "env") == 0 && argc > 3) {
		e = getenv(argv[2]);
		return !(e != 0 && strcmp(e, argv[3]) == 0);
	}
	if (strcmp(argv[1], "fdclosed") == 0 && argc > 2)
		return fcntl(atoi(argv[2]), F_GETFD) != -1;
	if (strcmp(argv[1], "fdopen") == 0 && argc > 2)
		return fcntl(atoi(argv[2]), F_GETFD) == -1;
	return 4;
}
