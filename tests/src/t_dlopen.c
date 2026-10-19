/*
 * t_dlopen.c -- shared objects built by our binutils: a library the program
 * links against, and one loaded with dlopen that calls back into the
 * program and that library.
 */
#include <stdio.h>
#include <dlfcn.h>
#include "t.h"

#define	SO	"/tests/libdlb.so"

int main_data = 1000;
extern int dla_data;
extern int dla_func(int);
extern int dla_dup(void);

int
main_cb(int v)
{
	return v * 10;
}

int
main()
{
	void *h;
	int (*f)(int);
	double (*sc)(double);
	int *d;

	t_init("dlopen", 60);
	t_check("dlopen.needed", dla_func(1) == 8 && dla_data == 7 && dla_dup() == 1,
		"dla_func(1) %d dla_data %d", dla_func(1), dla_data);
	h = dlopen(SO, RTLD_NOW);
	if (!h) {
		t_fail("dlopen.open", "%s", dlerror());
		return t_done();
	}
	t_pass("dlopen.open");
	f = (int (*)(int))dlsym(h, "dlb_func");
	sc = (double (*)(double))dlsym(h, "dlb_scale");
	d = (int *)dlsym(h, "dlb_data");
	if (!t_check("dlopen.sym", f && sc && d, "%s", dlerror()))
		return t_done();
	t_check("dlopen.data", *d == 42, "dlb_data %d", *d);
	t_check("dlopen.call", f(5) == 1144, "dlb_func(5) %d, want 1144", f(5));
	/* the program's copy of dla_data is the one the loaded object sees */
	dla_data = 8;
	t_check("dlopen.copy", f(5) == 1146, "dlb_func(5) %d, want 1146", f(5));
	t_check("dlopen.fp", sc(2.0) == 3.0, "dlb_scale(2) %g", sc(2.0));
	f = (int (*)(int))dlsym(h, "dlb_abs");
	t_check("dlopen.textrel", f && f(0) == 43, "dlb_abs %d", f ? f(0) : -1);
	t_check("dlopen.nosym", dlsym(h, "no_such_symbol") == 0, "found");
	t_check("dlopen.close", dlclose(h) == 0, "%s", dlerror());
	t_check("dlopen.reopen", (h = dlopen(SO, RTLD_LAZY)) != 0 &&
		(f = (int (*)(int))dlsym(h, "dlb_func")) != 0 && f(1) == 1106,
		"%s", h ? "dlb_func(1)" : dlerror());
	return t_done();
}
