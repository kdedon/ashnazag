#ifndef AMIGA_RTGSHARE_H
#define AMIGA_RTGSHARE_H

#define MIG_RTG_BASE 0x20000000UL
#define MIG_RTG_HEADER_SIZE 8192UL
#define MIG_RTG_MAP_SIZE MIG_RTG_HEADER_SIZE
#define MIG_RTG_MAGIC 0x4d525447U
#define MIG_RTG_VERSION 3U
/* the display session's memory: the card's video RAM */
#define MIG_RTG_VRAM 0x30000000UL
#define MIG_RTG_VRAM_MAX (16UL * 1024 * 1024)
/* RAM after the session's memory for bitmaps not shown: windows, icons */
#define MIG_RTG_EXTRA (4UL * 1024 * 1024)
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
    /*
     * Card memory: memory_size bytes from vram.  With copy clear it is the
     * display itself, rows vstride apart; with copy set it is RAM and the
     * host copies the shown screen to the display.
     */
    unsigned int vram, vstride, copy;
    /*
     * The pointer as the host drew it into the screen, under plock (taken
     * with tas by the host and by the card).  While pshown, the rectangle
     * x0,y0-x1,y1 of the screen at porigin holds pdrawn, and psave the
     * pixels it covered; a pixel still equal to pdrawn gets psave back.
     * The card sets pad when it took the pointer off, drawn when it drew
     * while track is set (cleared by the host before each copy), and the
     * host sets pwant when it found the lock taken: the card rings after
     * its drawing for each.
     */
    volatile unsigned char plock, pad, drawn, pwant;
    volatile unsigned int pshown;
    unsigned int porigin, pstride;
    int px0, py0, px1, py1;
    unsigned char psave[48][16], pdrawn[48][16];
    /* the display helper's wakeups and copy passes, for tests */
    volatile unsigned int wakes, copies;
    /*
     * track: the host copies the shown screen, so the card rings when it
     * draws.  ram: the host asks for the display's part of card memory in
     * RAM, while it copies another screen to the display; inram: where the
     * guest has it.
     */
    volatile unsigned int track, ram, inram;
};
typedef char mig_rtg_fits[sizeof(struct mig_rtg) <= MIG_RTG_HEADER_SIZE - 256 ? 1 : -1];

/* Writers publish an odd sequence, update controls, then publish an even one. */
#define MIG_RTG_BARRIER() __asm__ __volatile__("" : : : "memory")

void mig_rtg_init(struct mig_rtg *, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int);
int mig_rtg_snapshot(const volatile struct mig_rtg *, struct mig_rtg *,
    unsigned int, unsigned int);

#endif
