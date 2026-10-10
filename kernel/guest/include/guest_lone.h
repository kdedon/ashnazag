#ifndef GUEST_LONE_H
#define GUEST_LONE_H

struct guest_lone_stat {
	unsigned long entered, traps, reflected, privileged, hostcalls;
	unsigned long resets, stops, sigill;
};
extern void guest_lone_stats();
extern int guest_lone_init(), guest_lone_enter(), guest_lone_trap();

#endif
