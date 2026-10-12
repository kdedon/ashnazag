#ifndef GUEST_NATIVE_H
#define GUEST_NATIVE_H

#define GNC_VECTOR 45
#define GNC_ENTER 0x4e480001UL
#define GNC_LEAVE 0x4e480002UL
#define GNC_DEPTH 0x4e480003UL
#define GNC_MAXDEPTH 16

struct guest_proc;
struct guest_native {
	struct guest_proc *owner;
	unsigned int depth, saved;
};

extern int guest_native_init(), guest_native_control(), guest_native_active();
extern int guest_native_fork();
extern void guest_native_exit();

#endif
