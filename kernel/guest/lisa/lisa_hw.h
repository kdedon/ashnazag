#ifndef LISA_HW_H
#define LISA_HW_H

struct lisa_low_event {
	unsigned char key, ascii;
	unsigned short state, x, y;
	unsigned long time;
};

struct lisa_hw {
	unsigned char *surface;
	void *(*validate)(void *, unsigned long, unsigned long, int);
	void *opaque;
	int (*interrupted)(void *);
	unsigned char normal[160], busy[160], keys[16];
	struct lisa_low_event events[64], repeat_event;
	unsigned long head, count, state, depth, flags;
	unsigned long repeat_initial, repeat_next, repeat_due;
	unsigned long motion_due, busy_due, wall_offset;
	long x, y, dx, dy;
	int repeat_valid, busy_pending;
};

int lisa_hw_init(struct lisa_hw *, unsigned char *,
	void *(*)(void *, unsigned long, unsigned long, int), void *);
int lisa_hw_poll(struct lisa_hw *);
int lisa_hw_dispatch(struct lisa_hw *, unsigned long *, unsigned int);
int lisa_hw_clock(unsigned long *, unsigned long *);

#endif
