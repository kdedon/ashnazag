#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define TOSIOC_STRAM 123
#define TOSIOC_RAM 124
#define MB (1L << 20)

static int results[4], count, calls, policy;
static long requested[4];

static int
ioctl(int fd, int request, long size)
{
	assert(fd == 7 && request == policy && calls < count);
	requested[calls] = size;
	errno = results[calls++];
	return errno ? -1 : 0;
}

#include "../../guest/tos/tosram.h"

static void
reset(int a, int b, int c, int d, int n)
{
	results[0] = a; results[1] = b; results[2] = c; results[3] = d;
	count = n; calls = 0;
}

int
main(void)
{
	long size;
	for (policy = TOSIOC_STRAM; policy <= TOSIOC_RAM; policy++) {
	reset(0, 0, 0, 0, 1);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 0, policy) == 0 && size == 4 * MB && calls == 1);
	reset(ENOMEM, ENOMEM, 0, 0, 3);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 0, policy) == 0 && size == 2 * MB && calls == 3);
	assert(requested[0] == 4 * MB && requested[1] == 3 * MB && requested[2] == 2 * MB);
	reset(ENOMEM, ENOMEM, ENOMEM, ENOMEM, 4);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 0, policy) == -1 && errno == ENOMEM && size == MB && calls == 4);
	assert(requested[3] == MB);
	reset(EBUSY, 0, 0, 0, 1);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 0, policy) == -1 && errno == EBUSY && size == 4 * MB && calls == 1);
	reset(ENXIO, 0, 0, 0, 1);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 0, policy) == -1 && errno == ENXIO && size == 4 * MB && calls == 1);
	reset(ENOMEM, 0, 0, 0, 1);
	size = 4 * MB;
	assert(tos_ramalloc(7, &size, 1, policy) == -1 && errno == ENOMEM && size == 4 * MB && calls == 1);
	}
	puts("[OK] both ST-RAM policies: default, fallback, exhaustion, errors, explicit size");
	return 0;
}
