/* poll(2) events and the driver poll head. */
#ifndef _SYS_POLL_H
#define _SYS_POLL_H

struct pollfd {
	int	fd;
	short	events;
	short	revents;
};

#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020
#define POLLRDNORM	0x0040
#define POLLRDBAND	0x0080
#define POLLWRBAND	0x0100
#define POLLWRNORM	POLLOUT

/* one per pollable object; pollwakeup() wakes its pollers */
struct pollhead {
	struct polldat	*ph_list;
	char		_ph0[4];
	short		ph_events;
	char		_ph1[22];
};

extern void pollwakeup();

#endif
