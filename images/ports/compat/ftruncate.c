/* ftruncate(2) through fcntl F_FREESP, which libc.so.1 lacks. */
#include <sys/types.h>
#include <fcntl.h>

int ftruncate(int fd, off_t len)
{
	struct flock fl;

	fl.l_type = F_WRLCK;
	fl.l_whence = 0;
	fl.l_start = len;
	fl.l_len = 0;
	return fcntl(fd, F_FREESP, &fl);
}
