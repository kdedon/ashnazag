#ifndef MIGVIDEO_H
#define MIGVIDEO_H
#define MIG_WIDTH 640
#define MIG_HEIGHT 512
struct migframe {
    int width, height;
    unsigned short rgb[MIG_WIDTH * MIG_HEIGHT];
};
int mig_render(const unsigned short *, const unsigned char *, unsigned long,
    struct migframe *);
#endif
