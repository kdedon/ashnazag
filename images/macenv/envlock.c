/*
 * envlock -- run a command as the one session of a Mac System Folder.
 *
 *	envlock file command [arg ...]
 *	envlock -t file
 *
 * The command inherits a write lock on file, which must exist, and
 * holds it until it exits.  -t only checks that no session holds it.
 */
#include <stdlib.h>
#include <unistd.h>
#include "../../kernel/guest/include/envroot.h"

int
main(argc, argv)
	int argc;
	char **argv;
{
	int test = argc == 3 && strcmp(argv[1], "-t") == 0;
	char root[1024], *f, *s;

	if (argc < 3) {
		fprintf(stderr, "usage: envlock file command [arg ...] | envlock -t file\n");
		return 2;
	}
	f = argv[1 + test];
	sprintf(root, "%.1000s", f);
	if ((s = strrchr(root, '/')) != 0 && s > root)
		*s = 0;
	if (envlockfile("startmac", root, f, 0) == -1)
		return 1;
	if (test)
		return 0;
	execvp(argv[2], argv + 2);
	fprintf(stderr, "envlock: %s: %s\n", argv[2], strerror(errno));
	return 127;
}
