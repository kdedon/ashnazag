#ifndef ENVMEM_H
#define ENVMEM_H

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* A fixed virtual region; a supplied device owns DMA backing and lifetime. */
static int
envmem_region(address, size, shared, device)
unsigned long address, size;
int shared, device;
{
	int fd = device, own = device < 0, saved, result;
	if (!size || address + size < address) {
		errno = EINVAL;
		return -1;
	}
	if (own && (fd = open("/dev/zero", O_RDWR)) < 0)
		return -1;
	result = mmap((caddr_t)address, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	    (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, 0) == (caddr_t)-1 ? -1 : 0;
	saved = errno;
	if (own)
		close(fd);
	errno = saved;
	return result;
}
#endif
