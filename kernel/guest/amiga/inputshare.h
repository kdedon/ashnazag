#ifndef MIG_INPUTSHARE_H
#define MIG_INPUTSHARE_H
#define MIG_INPUT_BASE 0x21000000UL
#define MIG_INPUT_MAP_SIZE 4096UL
#define MIG_INPUT_MAGIC 0x4d494e50U
#define MIG_INPUT_VERSION 1U
#define MIG_INPUT_COUNT 256U
#define MIG_INPUT_BARRIER() __asm__ __volatile__("" : : : "memory")
struct mig_input_event {
    unsigned short type, code, qualifier, reserved;
    short x, y;
};
struct mig_input {
    unsigned int magic, version;
    volatile unsigned int head, tail, reset, ack, ready;
    volatile unsigned int generation;
    struct mig_input_event event[MIG_INPUT_COUNT];
    volatile unsigned int doorbell;   /* set before the host raises PORTS */
};
struct mig_input_state {
    unsigned int generation;
    unsigned char keys[128], sources[128];
    unsigned short qualifier;
};
void mig_input_init(struct mig_input *);
void mig_input_reset(struct mig_input *, struct mig_input_state *);
int mig_input_event(struct mig_input *, struct mig_input_state *,
    unsigned int, unsigned int, unsigned int, unsigned int, long);
int mig_input_translate(unsigned int, unsigned int);
#endif
