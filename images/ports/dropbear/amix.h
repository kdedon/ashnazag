/* What dropbear expects of the system and AMIX lacks */
#include <sys/types.h>
typedef long suseconds_t;
struct timespec {
	time_t tv_sec;
	long tv_nsec;
};
#define SHUT_RD		0
#define SHUT_WR		1
#define SHUT_RDWR	2
#define IOV_MAX		16
#define nanosleep(req, rem)	usleep((req)->tv_sec * 1000000 + (req)->tv_nsec / 1000)
#define inet_aton(cp, ap)	(((ap)->s_addr = inet_addr(cp)) != (unsigned long)-1)
#define USE_DEV_PTMX	1
/* a non-blocking connect is not reported complete */
#define BLOCKING_CONNECT
