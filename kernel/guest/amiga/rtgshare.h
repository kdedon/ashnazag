#ifndef AMIGA_RTGSHARE_H
#define AMIGA_RTGSHARE_H

#define MIG_RTG_BASE 0x20000000UL
#define MIG_RTG_HEADER_SIZE 4096UL
#define MIG_RTG_PIXELS (MIG_RTG_BASE + MIG_RTG_HEADER_SIZE)
#define MIG_RTG_SIZE (4UL * 1024 * 1024)
#define MIG_RTG_MAP_SIZE (MIG_RTG_HEADER_SIZE + MIG_RTG_SIZE)
#define MIG_RTG_MAGIC 0x4d525447U
#define MIG_RTG_VERSION 1U
#define MIG_RTG_INDEX8 1U
#define MIG_RTG_NATIVE 0U
#define MIG_RTG_VISIBLE 1U
#define MIG_RTG_BLANK 2U

/* All words use native big-endian byte order on the guest. */
struct mig_rtg {
    unsigned int magic, version, header_size, memory_size;
    unsigned int max_width, max_height, format;
    volatile unsigned int seq;
    unsigned int on, width, height, stride, offset;
    unsigned short palette[256][3];
    /*
     * The pointer as a hardware sprite: the host sets cursor and draws it
     * over the screen; the card writes the rest between odd and even cseq.
     * cimg holds colour numbers 0 (clear) to 3, crgb colours 1 to 3.
     */
    unsigned int cursor;
    volatile unsigned int cseq;
    unsigned int con;
    int cx, cy;
    unsigned int cw, ch;
    unsigned short crgb[4][3];
    unsigned char cimg[48][16];
};

/* Writers publish an odd sequence, update controls, then publish an even one. */
#define MIG_RTG_BARRIER() __asm__ __volatile__("" : : : "memory")

void mig_rtg_init(struct mig_rtg *, unsigned int, unsigned int);
int mig_rtg_snapshot(const volatile struct mig_rtg *, struct mig_rtg *,
    unsigned int, unsigned int);

#endif
