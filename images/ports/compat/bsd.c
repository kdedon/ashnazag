/* BSD and XENIX calls missing from the SVR4 libc. */
#include <poll.h>

static unsigned long seed = 1;

long random(void)
{
	seed = seed * 1103515245 + 12345;
	return (long)(seed >> 1) & 0x7fffffff;
}

void srandom(unsigned s)
{
	seed = s;
}

/* sleep ms milliseconds */
long nap(long ms)
{
	poll((struct pollfd *)0, 0, (int)ms);
	return 0;
}
