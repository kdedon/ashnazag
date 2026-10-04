/*
 * auxtest.c -- a small library for mkaslm's tests: initialised data with
 * pointers, a table of functions, calls within Main and into %A5Init.
 */

static char greet[] = "hello from Main";
static char *msgs[] = { greet, "second" };
static long counter = 5;
static long zeroed[16];

static long
twice(x)
	long x;
{
	return 2 * x;
}

static long (*fns[])() = { twice };

long
AUXTestAdd(a, b)
	long a, b;
{
	counter++;
	return (*fns[0])(a) / 2 + b;
}

char *
AUXTestMsg(i)
	long i;
{
	return msgs[i & 1];
}

long
AUXTestCount()
{
	zeroed[3] += twice(counter) / 2;
	return zeroed[3];
}
