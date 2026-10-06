#ifndef MIG_SNDSHARE_H
#define MIG_SNDSHARE_H
/*
 * The AHI driver's output: 16-bit big-endian frames in a ring the guest
 * fills and the sound helper drains to the host's sound service.  The
 * guest rings MIG_SND_BELL when play starts or stops; the helper raises
 * PORTS (setting doorbell first) when the ring holds less than want.
 */
#define MIG_SND_BASE 0x23000000UL
#define MIG_SND_HEADER_SIZE 4096UL
#define MIG_SND_RING (256UL * 1024)
#define MIG_SND_MAP_SIZE (MIG_SND_HEADER_SIZE + MIG_SND_RING)
#define MIG_SND_MAGIC 0x4d534e44U
#define MIG_SND_VERSION 1U
#define MIG_SND_BELL 0x00f7fff8UL
#define MIG_SND_BARRIER() __asm__ __volatile__("" : : : "memory")

struct mig_snd {
    unsigned int magic, version;
    volatile unsigned int ready;      /* host: the helper runs */
    volatile unsigned int play;       /* guest: between Start and Stop */
    volatile unsigned int gen;        /* guest: bumped by Start */
    volatile unsigned int rate;       /* guest: Hz */
    volatile unsigned int chans;      /* guest: 1 or 2 */
    volatile unsigned int head;       /* guest: bytes written */
    volatile unsigned int tail;       /* host: bytes taken */
    volatile unsigned int want;       /* host: bytes to keep queued */
    volatile unsigned int doorbell;   /* host: set before it raises PORTS */
    volatile unsigned int frames;     /* host: frames taken */
    volatile unsigned int kicks;      /* host: PORTS raised */
    volatile unsigned int passes;     /* guest: player calls */
};
int mig_snd_helper(int, int, int);
#endif
