static int
tos_ramalloc(fd, size, exact, request)
int fd, exact, request;
long *size;
{
	for (;;) {
		if (ioctl(fd, request, *size) == 0)
			return 0;
		if (errno != ENOMEM || exact || *size <= (1L << 20))
			return -1;
		*size -= 1L << 20;
	}
}
