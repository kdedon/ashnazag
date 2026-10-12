#ifndef LISA_RUNTIME_H
#define LISA_RUNTIME_H
#define LP_MAJOR 64
#define LP_ENTER 0x4c4e0001
#define LP_LEAVE 0x4c4e0002
#define LP_INFO 0x4c4e0003
#define LP_PUBLISH 0x4c4e0004
#define LP_LOCK 0x4c4e0005
#define LP_PROTECT 0x4c4e0007
#define LP_UNLOCK 0x4c4e0006
#define LP_RETURN 0x4c4e0010UL
#define LP_VERSION 1
#define LP_CODE 1
#define LP_WRITE 2
#define LP_EXEC 4
#define LP_MAXREGIONS 60
struct lisa_region { unsigned long base, size, flags, mapped, mapped_size; };
struct lisa_config {
	unsigned long version, count, handler, frame, stack;
	struct lisa_region region[LP_MAXREGIONS];
};
struct lisa_frame { unsigned long reg[16]; unsigned short sr; unsigned long pc; unsigned short vector; } __attribute__((packed));
typedef char lisa_frame_size[sizeof(struct lisa_frame)==72?1:-1];
extern const char *lisa_program;
void *lisa_pointer(unsigned long, unsigned long, int);
int lisa_executable(unsigned long, unsigned long);
int lisa_protect(unsigned long, unsigned long, int);
int lisa_wait_tick(void);
int lisa_lock_code(unsigned long);
int lisa_resident(void);
int lisa_slot_resident(unsigned long, unsigned long);
void lisa_set_init(unsigned long);
int lisa_set_terminate(unsigned long, unsigned long);
int lisa_terminate(const unsigned char *);
#endif
