#include <stdio.h>
#include <stdlib.h>
#include "kinc.h"
#include "guest_native.h"

struct allocation { struct guest_proc gp; struct guest_native state, other; };
static unsigned int checks;
#define CHECK(e) do { checks++; if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); } } while (0)

static int
control(struct allocation *a, unsigned long operation)
{
	struct test_frame r;
	memset(&r, 0, sizeof r);
	r.d[0] = operation;
	CHECK(guest_native_control(&a->gp, &a->state, (char *)&r) == 0);
	return r.d[0];
}

int
main(void)
{
	struct allocation a, b;
	struct test_frame r;
	int i;
	unsigned int saved;

	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	a.gp.gp_size = b.gp.gp_size = sizeof a;
	CHECK(guest_native_init(&a.gp, &a.state) == 0);
	CHECK(guest_native_init(&a.gp, (struct guest_native *)((char *)&a + sizeof a - 2)) == EINVAL);
	CHECK(guest_native_init(&a.gp, (struct guest_native *)&a.gp) == EINVAL);
	CHECK(guest_native_init(&a.gp, (struct guest_native *)((char *)&a.state + 1)) == EINVAL);
	a.gp.gp_flags = GPF_FTRAP13;
	CHECK(guest_native_init(&a.gp, &a.state) == EBUSY);
	a.gp.gp_flags = GPF_ALINE | GPF_FTRAP | GPF_PRIV | GPF_QUIET | GPF_AWATCH;
	saved = a.gp.gp_flags;
	CHECK(control(&a, GNC_LEAVE) == EINVAL);
	CHECK(control(&a, GNC_ENTER) == 0);
	CHECK(a.gp.gp_flags == (GPF_NCALL | GPF_QUIET | GPF_AWATCH));
	CHECK(guest_native_active(&a.gp, &a.state));
	CHECK(guest_native_init(&a.gp, &a.other) == EBUSY);
	for (i = 1; i < GNC_MAXDEPTH; i++)
		CHECK(control(&a, GNC_ENTER) == 0);
	CHECK(control(&a, GNC_ENTER) == EBUSY);
	CHECK(control(&a, GNC_DEPTH) == GNC_MAXDEPTH);
	b.gp.gp_flags = a.gp.gp_flags;
	CHECK(guest_native_fork(&a.gp, &b.gp, &a.state, &b.state) == 0);
	CHECK(b.state.owner == &b.gp && b.state.depth == GNC_MAXDEPTH);
	CHECK(guest_native_active(&b.gp, &b.state));
	guest_native_exit(&b.gp, &b.state);
	CHECK(b.gp.gp_flags == saved && !b.state.owner && !b.state.depth);
	for (i = 0; i < GNC_MAXDEPTH; i++)
		CHECK(control(&a, GNC_LEAVE) == 0);
	CHECK(a.gp.gp_flags == saved && !guest_native_active(&a.gp, &a.state));
	CHECK(control(&a, GNC_LEAVE) == EINVAL);
	memset(&r, 0, sizeof r);
	r.d[0] = 0x1234;
	CHECK(guest_native_control(&a.gp, &a.state, (char *)&r) == 1 && r.d[0] == 0x1234);
	r.d[0] = GNC_ENTER;
	r.sr = 0x2000;
	CHECK(!guest_native_control(&a.gp, &a.state, (char *)&r) && r.d[0] == EINVAL);
	a.state.owner = &b.gp;
	CHECK(control(&a, GNC_ENTER) == EINVAL);
	a.state.owner = &a.gp;
	CHECK(control(&a, GNC_ENTER) == 0);
	a.gp.gp_flags = 0;
	CHECK(control(&a, GNC_LEAVE) == EINVAL);
	CHECK(guest_native_init(&a.gp, &a.state) == 0 && !a.state.depth);
	a.gp.gp_flags = GPF_FTRAP;
	CHECK(control(&a, GNC_ENTER) == 0);
	guest_native_exit(&a.gp, &a.state);
	CHECK(a.gp.gp_flags == GPF_FTRAP && !a.state.owner && !a.state.depth);
	printf("PASS nativecall %u ownership, bounds, nesting, flags, fork, lifetime checks\n", checks);
	return 0;
}
