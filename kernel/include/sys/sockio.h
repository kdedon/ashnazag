/* Socket and interface ioctls. */
#ifndef _SYS_SOCKIO_H
#define _SYS_SOCKIO_H

#include <sys/ioccom.h>

#define SIOCSPGRP	_IOW('s', 8, int)
#define SIOCGPGRP	_IOR('s', 9, int)
#define SIOCSIFFLAGS	_IOW('i', 16, struct ifreq)
#define SIOCGIFFLAGS	_IOWR('i', 17, struct ifreq)
#define SIOCGIFCONF	_IOWR('i', 20, struct ifconf)

#endif
