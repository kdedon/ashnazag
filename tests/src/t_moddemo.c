/* t_moddemo.c -- does a write to a shared file mapping, after a read of the
 * same page, reach the file once the page is synced and dropped? */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "t.h"

static int
one(path, readfirst, i)
char *path;
int readfirst, i;
{
	char buf[4096], *m, c;
	int fd, r;

	memset(buf, 'a', sizeof buf);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, buf, sizeof buf);
	fsync(fd);
	m = mmap((caddr_t)0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)0);
	if (m == (char *)-1)
		return -1;
	/* drop the page so the next touch is a fresh fault */
	msync(m, 4096, MS_SYNC | MS_INVALIDATE);
	if (readfirst)
		c = *(volatile char *)(m + 100);
	m[100] = 'B' + i % 20;
	r = msync(m, 4096, MS_SYNC | MS_INVALIDATE);
	munmap(m, 4096);
	close(fd);
	fd = open(path, O_RDONLY);
	read(fd, buf, sizeof buf);
	close(fd);
	return buf[100] == 'B' + i % 20 ? 0 : 1;
}

int
main()
{
	int i, lostr = 0, lostw = 0;

	t_init("moddemo", 120);
	for (i = 0; i < 20; i++) {
		lostw += one("/tmp/md", 0, i);
		lostr += one("/tmp/md", 1, i);
	}
	t_check("write_only", lostw == 0, "%d of 20 writes lost", lostw);
	t_check("read_then_write", lostr == 0, "%d of 20 writes lost", lostr);
	unlink("/tmp/md");
	return t_done();
}
