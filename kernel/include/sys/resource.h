/* Resource limits. */
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#define RLIMIT_CPU	0
#define RLIMIT_FSIZE	1
#define RLIMIT_DATA	2
#define RLIMIT_STACK	3
#define RLIMIT_CORE	4
#define RLIMIT_NOFILE	5
#define RLIMIT_VMEM	6
#define RLIM_NLIMITS	7
#define RLIM_INFINITY	0x7fffffff

typedef unsigned long rlim_t;
struct rlimit {
	rlim_t	rlim_cur;
	rlim_t	rlim_max;
};

#endif
