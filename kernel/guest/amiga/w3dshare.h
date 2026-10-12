#ifndef AMIGA_W3DSHARE_H
#define AMIGA_W3DSHARE_H

/*
 * Warp3D.library's way into startmig: the host functions it calls, one
 * per library vector, on the host stack, under the library's semaphore.
 */
#define MIG_W3D_BASE 0x25000000UL
#define MIG_W3D_MAP_SIZE 4096UL
#define MIG_W3D_MAGIC 0x57334448U
#define MIG_W3D_VERSION 1U
#define MIG_W3D_NFN 84
#define MIG_W3D_STACK (128UL * 1024)

/* All words big-endian; the library uses the byte offsets in comments. */
struct mig_w3d {
    unsigned int magic, version, count;	/* 0, 4, 8 */
    unsigned int stack;			/* 12: top of the host stack */
    volatile unsigned int ring;		/* 16: the library rings the display after the call */
    /*
     * 20: the BitMap the call names, as the library found it through
     * Picasso96API: bitmap, memory, bytes per row, RGBFB format, width,
     * height; bitmap 0 when none
     */
    volatile unsigned int bm[6];
    unsigned int spoilt;		/* 44: called when a call changed ExecBase */
    unsigned int pad[4];
    unsigned int fn[MIG_W3D_NFN];	/* 64 */
    volatile unsigned int calls[MIG_W3D_NFN];	/* 400 */
};

typedef char mig_w3d_fits[(sizeof(struct mig_w3d) <= MIG_W3D_MAP_SIZE) ? 1 : -1];

void w3d_init(struct mig_w3d *);
extern int w3d_fbfd;
void w3d_text(void);
void fill32(void *, unsigned long, unsigned long);

#endif
