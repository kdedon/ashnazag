/*
 * fscheck -- cpmfs on the build host: images in each format, filled
 * with the given files and a few made here; fscheck.py reads them back.
 *
 *	fscheck dir file...
 *
 * Writes dir/<format>.img and dir/fs.lst (image, CP/M name, source).
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "cpmfs.h"

static char *dir;
static FILE *lst;

static char *
slurp(path, lenp)
	char *path;
	long *lenp;
{
	FILE *fp = fopen(path, "rb");
	char *b;

	if (fp == 0 || fseek(fp, 0L, 2) < 0 || (*lenp = ftell(fp)) < 0 ||
	    (b = malloc(*lenp + 1)) == 0 || fseek(fp, 0L, 0) < 0 ||
	    fread(b, 1, *lenp, fp) != *lenp) {
		perror(path);
		exit(1);
	}
	fclose(fp);
	return b;
}

static void
made(name, len, seed)
	char *name;
	long len;
	int seed;
{
	char path[1100];
	FILE *fp;
	long i;

	sprintf(path, "%s/%s", dir, name);
	if ((fp = fopen(path, "wb")) == 0) {
		perror(path);
		exit(1);
	}
	for (i = 0; i < len; i++)
		putc((int)((i * 131 + (i >> 9) * 7 + seed) & 255), fp);
	fclose(fp);
}

/* each file that fits; all must when all is set */
static void
fill(img, d, files, nf, all)
	char *img;
	struct cpmimg *d;
	char **files;
	int nf, all;
{
	char n[11], *b, *base;
	long len;
	int i, in = 0;

	for (i = 0; i < nf; i++) {
		base = strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i];
		if (cpm_name(base, n) < 0)
			continue;
		b = slurp(files[i], &len);
		if (cpm_put(d, 0, n, b, len) == 0) {
			fprintf(lst, "%s %.11s %s\n", img, n, files[i]);
			in++;
		} else if (all) {
			fprintf(stderr, "fscheck: %s: %s refused\n", img, base);
			exit(1);
		}
		free(b);
	}
	if (cpm_put(d, 0, "EMPTY   TXT", "", 0L) == 0)
		fprintf(lst, "%s EMPTY   TXT %s/EMPTY.TXT\n", img, dir);
	if (cpm_put(d, 0, "EMPTY   TXT", "", 0L) == 0) {
		fprintf(stderr, "fscheck: %s: a name went in twice\n", img);
		exit(1);
	}
	printf("[ok] fscheck %s: %d files\n", img, in);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	static long sizes[] = { 8L << 20, 256256L, 256L << 10, 1L << 20 };
	char path[1100], img[32], *files[1024], *small = 0;
	struct cpmimg d;
	int i, nf, k;

	if (argc < 2 || argc > 1000)
		return 2;
	dir = argv[1];
	sprintf(path, "%s/fs.lst", dir);
	if ((lst = fopen(path, "w")) == 0)
		return 1;
	made("EMPTY.TXT", 0L, 0);
	made("BIG.DAT", 600000L, 1);
	made("ODD.DAT", 4097L, 2);
	sprintf(path, "%s/BIG.DAT", dir);
	files[0] = strdup(path);
	sprintf(path, "%s/ODD.DAT", dir);
	files[1] = strdup(path);
	for (nf = 2, i = 2; i < argc; i++)
		files[nf++] = argv[i];
	if (cpm_name("a.b.c", path) == 0 || cpm_name("toolongname.x", path) == 0 ||
	    cpm_name("x.long", path) == 0 || cpm_name(".x", path) == 0 ||
	    cpm_name("pip.rel", path) < 0 || memcmp(path, "PIP     REL", 11)) {
		fprintf(stderr, "fscheck: cpm_name\n");
		return 1;
	}
	for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
		memset((char *)&d, 0, sizeof d);
		if (cpm_format(sizes[k], &d.f) < 0) {
			fprintf(stderr, "fscheck: size %ld refused\n", sizes[k]);
			return 1;
		}
		sprintf(img, "f%ld.img", sizes[k]);
		sprintf(path, "%s/%s", dir, img);
		if (k == 3) {			/* in memory, as a copied folder */
			d.mem = small = malloc(sizes[k]);
			memset(small, 0xe5, sizes[k]);
		} else if ((d.fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0 ||
		    cpm_mkfs(&d) < 0) {
			perror(path);
			return 1;
		}
		fill(img, &d, files, nf, k == 0);
		if (k == 3) {
			if ((d.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 ||
			    write(d.fd, small, sizes[k]) != sizes[k])
				return 1;
			free(small);
		}
		close(d.fd);
	}
	if (cpm_format(300000L, &d.f) == 0 || cpm_format(16L << 20, &d.f) == 0) {
		fprintf(stderr, "fscheck: an unknown size was taken\n");
		return 1;
	}
	fclose(lst);
	return 0;
}
