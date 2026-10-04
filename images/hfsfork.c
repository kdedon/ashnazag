/*
 * hfsfork - replace one fork of an existing file on an HFS volume in place,
 * so the file keeps its catalog node ID (aliases to it stay valid).
 *
 *	usage:	hfsfork image partno hfspath d|r infile
 *
 * partno counts Apple_HFS partitions from 1, as hmount does.
 * Build: cc -I<libhfs> -o hfsfork hfsfork.c <libhfs>/libhfs.a
 */
#include <stdio.h>
#include <stdlib.h>
#include "hfs.h"

static char *progname;

static void
fatal(msg, arg)
char	*msg;
char	*arg;
{
	fprintf(stderr, "%s: %s", progname, msg);
	if (arg)
		fprintf(stderr, " %s", arg);
	if (hfs_error)
		fprintf(stderr, " (%s)", hfs_error);
	fputc('\n', stderr);
	exit(1);
}

int
main(argc, argv)
int	argc;
char	**argv;
{
	FILE		*fp;
	char		*buf;
	long		len;
	hfsvol		*vol;
	hfsfile		*file;
	hfsdirent	ent;
	int		fork;

	progname = argv[0];
	if (argc != 6 || (argv[4][0] != 'd' && argv[4][0] != 'r') ||
	    argv[4][1] != '\0')
		fatal("usage: hfsfork image partno hfspath d|r infile",
		    (char *)0);
	fork = argv[4][0] == 'r';

	if ((fp = fopen(argv[5], "rb")) == NULL)
		fatal("cannot open", argv[5]);
	fseek(fp, 0L, SEEK_END);
	len = ftell(fp);
	rewind(fp);
	if ((buf = malloc(len ? len : 1)) == NULL)
		fatal("out of memory", (char *)0);
	if (fread(buf, 1, len, fp) != (size_t)len)
		fatal("cannot read", argv[5]);
	fclose(fp);

	if ((vol = hfs_mount(argv[1], atoi(argv[2]), HFS_MODE_RDWR)) == NULL)
		fatal("cannot mount", argv[1]);
	if ((file = hfs_open(vol, argv[3])) == NULL)
		fatal("cannot open", argv[3]);
	if (hfs_setfork(file, fork) == -1)
		fatal("cannot select fork of", argv[3]);
	if (hfs_seek(file, 0L, HFS_SEEK_SET) == (unsigned long)-1)
		fatal("cannot seek", argv[3]);
	if (hfs_write(file, buf, (unsigned long)len) != (unsigned long)len)
		fatal("cannot write", argv[3]);
	if (hfs_truncate(file, (unsigned long)len) == -1)
		fatal("cannot truncate", argv[3]);
	if (hfs_fstat(file, &ent) == -1)
		fatal("cannot stat", argv[3]);
	if (hfs_close(file) == -1)
		fatal("cannot close", argv[3]);
	if (hfs_umount(vol) == -1)
		fatal("cannot unmount", argv[1]);

	printf("%s: %s fork %ld bytes, catalog id %lu\n", argv[3],
	    fork ? "resource" : "data",
	    fork ? (long)ent.u.file.rsize : (long)ent.u.file.dsize,
	    (unsigned long)ent.cnid);
	free(buf);
	return 0;
}
