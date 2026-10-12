#ifndef NHC_HOST_KINC
#define NHC_HOST_KINC
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include "guest.h"
struct test_frame { long d[8]; unsigned short sr; };
#undef GR_D
#undef GR_SR
#define GR_D(r, n) (((struct test_frame *)(r))->d[n])
#define GR_SR(r) (((struct test_frame *)(r))->sr)
#endif
