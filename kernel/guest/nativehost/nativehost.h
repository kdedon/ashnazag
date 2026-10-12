#ifndef NATIVEHOST_H
#define NATIVEHOST_H

#include <sys/types.h>
#include "dsio.h"

struct nh_display {
    int framebuffer, keyboard, mouse;
    void *mapping;
    struct fbinfo info;
};

struct nh_clock {
    unsigned long ticks;
    long frequency;
};

int nh_native_begin(void), nh_native_end(void), nh_native_depth(void);

void nh_display_init(struct nh_display *);
int nh_display_open(struct nh_display *, const char *, const char *, int);
int nh_display_close(struct nh_display *);
int nh_input(int, struct inev *);
int nh_clock_read(struct nh_clock *);
void *nh_memory(unsigned int, int);
long nh_file_transfer(int, off_t, void *, unsigned int, int);

#endif
