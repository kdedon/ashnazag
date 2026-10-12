#include "kinc.h"
#include "guest_native.h"

#define GNC_SAVED (GPF_QUIET | GPF_ALINE | GPF_PRIV | GPF_FTRAP)

static int
gnc_bounds(gp, state)
struct guest_proc *gp;
struct guest_native *state;
{
	unsigned long base, address, offset;

	if (!gp || !state || gp->gp_size < sizeof *gp + sizeof *state)
		return 0;
	base = (unsigned long)(gp + 1);
	address = (unsigned long)state;
	if (address < base || (address & 1))
		return 0;
	offset = address - base;
	return offset <= gp->gp_size - sizeof *gp - sizeof *state;
}

int
guest_native_init(gp, state)
struct guest_proc *gp;
struct guest_native *state;
{
	if (!gnc_bounds(gp, state))
		return EINVAL;
	if (gp->gp_flags & (GPF_NCALL | GPF_FTRAP13))
		return EBUSY;
	bzero((caddr_t)state, sizeof *state);
	state->owner = gp;
	return 0;
}

int
guest_native_active(gp, state)
struct guest_proc *gp;
struct guest_native *state;
{
	return gnc_bounds(gp, state) && state->owner == gp && state->depth &&
		(gp->gp_flags & GPF_NCALL);
}

int
guest_native_control(gp, state, r)
struct guest_proc *gp;
struct guest_native *state;
char *r;
{
	unsigned long operation = (unsigned long)GR_D(r, 0);
	int error = 0;

	if (operation != GNC_ENTER && operation != GNC_LEAVE && operation != GNC_DEPTH)
		return 1;
	if (!gnc_bounds(gp, state) || state->owner != gp || (GR_SR(r) & 0x2000))
		error = EINVAL;
	else if (gp->gp_flags & GPF_FTRAP13)
		error = EBUSY;
	else if ((state->depth && !(gp->gp_flags & GPF_NCALL)) ||
		(!state->depth && (gp->gp_flags & GPF_NCALL)))
		error = EINVAL;
	else if (operation == GNC_DEPTH) {
		GR_D(r, 0) = state->depth;
		return 0;
	} else if (operation == GNC_ENTER) {
		if (state->depth >= GNC_MAXDEPTH)
			error = EBUSY;
		else {
			if (!state->depth) {
				state->saved = gp->gp_flags & GNC_SAVED;
				gp->gp_flags &= ~GNC_SAVED;
				gp->gp_flags |= GPF_NCALL | GPF_QUIET;
			}
			state->depth++;
		}
	} else if (!state->depth)
		error = EINVAL;
	else if (!--state->depth) {
		gp->gp_flags &= ~(GNC_SAVED | GPF_NCALL);
		gp->gp_flags |= state->saved;
		state->saved = 0;
	}
	GR_D(r, 0) = error;
	return 0;
}

int
guest_native_fork(parent, child, source, destination)
struct guest_proc *parent, *child;
struct guest_native *source, *destination;
{
	if (!gnc_bounds(parent, source) || source->owner != parent ||
		!gnc_bounds(child, destination))
		return EINVAL;
	bcopy((caddr_t)source, (caddr_t)destination, sizeof *source);
	destination->owner = child;
	return 0;
}

void
guest_native_exit(gp, state)
struct guest_proc *gp;
struct guest_native *state;
{
	if (!gnc_bounds(gp, state) || state->owner != gp)
		return;
	if (state->depth) {
		gp->gp_flags &= ~(GNC_SAVED | GPF_NCALL);
		gp->gp_flags |= state->saved;
	}
	bzero((caddr_t)state, sizeof *state);
}
