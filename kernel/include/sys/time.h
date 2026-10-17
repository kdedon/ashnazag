/* Time values. */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>

struct timeval {
	long	tv_sec;
	long	tv_usec;
};

typedef struct timestruc {
	time_t	tv_sec;
	long	tv_nsec;
} timestruc_t;

extern timestruc_t hrestime;	/* time of day */

#endif
