/*
 * mcat -- cat and ls: "mcat file..." copies files to stdout,
 * "mcat -l dir" lists names with their size and type.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

int
main(int argc, char **argv)
{
	char buf[512], p[512];
	struct dirent *e;
	struct stat sb;
	DIR *d;
	int i, fd, n;

	if (argc == 3 && strcmp(argv[1], "-l") == 0) {
		if ((d = opendir(argv[2])) == NULL) {
			perror(argv[2]);
			return 1;
		}
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.')
				continue;
			sprintf(p, "%s/%s", argv[2], e->d_name);
			if (lstat(p, &sb) < 0) {
				perror(p);
				return 1;
			}
			printf("%s %ld %c\n", e->d_name, (long)sb.st_size,
			    S_ISDIR(sb.st_mode) ? 'd' : S_ISLNK(sb.st_mode) ? 'l' : '-');
		}
		closedir(d);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		if ((fd = open(argv[i], O_RDONLY)) < 0) {
			perror(argv[i]);
			return 1;
		}
		while ((n = read(fd, buf, sizeof buf)) > 0)
			write(1, buf, n);
		close(fd);
	}
	return 0;
}
