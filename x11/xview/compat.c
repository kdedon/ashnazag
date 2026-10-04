/* compat.c -- BSD calls XView uses that AMIX's libc lacks. */
#include <stdlib.h>
#include <string.h>

char *
index(s, c)
	char *s;
	int c;
{
	return strchr(s, c);
}

char *
rindex(s, c)
	char *s;
	int c;
{
	return strrchr(s, c);
}

long
random()
{
	return lrand48();
}

void
srandom(seed)
	unsigned seed;
{
	srand48((long)seed);
}

char *getcwd();

char *
getwd(buf)
	char *buf;
{
	if (getcwd(buf, 1024) == 0) {
		strcpy(buf, "getwd: cannot get current directory");
		return 0;
	}
	return buf;
}

int
killpg(pgrp, sig)
	int pgrp, sig;
{
	return kill(-pgrp, sig);
}

void
bcopy(from, to, n)
	char *from, *to;
	int n;
{
	memmove(to, from, n);
}

void
bzero(s, n)
	char *s;
	int n;
{
	memset(s, 0, n);
}
