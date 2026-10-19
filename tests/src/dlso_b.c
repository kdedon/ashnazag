/*
 * libdlb.so: loaded by t_dlopen.  Takes symbols from the program and from
 * the program's library, whose dla_dup comes before this object's own.
 */
#include <stdio.h>
#include <string.h>

extern int main_cb(int);
extern int dla_func(int);
extern int dla_data;

int dlb_data = 42;
int *dlb_ptr = &dlb_data;
const char *dlb_str = "shared";
static int tab[4] = { 1, 2, 3, 4 };
static double scale = 1.5;

static int
add(int a)
{
	return a + tab[2];
}

int (*dlb_fp)(int) = add;

int
dla_dup(void)
{
	return 2;
}

/* (x + 3) * 10 + 7 + 7 + 42 + strlen("shared x") + 1000 */
int
dlb_func(int x)
{
	char b[64];

	sprintf(b, "%s %d", dlb_str, x);
	return main_cb(dlb_fp(x)) + dla_func(0) + dla_data + *dlb_ptr +
		(int)strlen(b) + dla_dup() * 1000;
}

double
dlb_scale(double v)
{
	return v * scale;
}
