#ifndef NATIVEHOST_UI_H
#define NATIVEHOST_UI_H

#include "dsio.h"

/* Packed pixels use most-significant bits and bytes first. Values are native
 * palette indices or true-color mask values; the display owns color policy.
 */
struct nh_surface {
    unsigned char *pixels;
    unsigned long width, height, stride, depth;
};

/* The mapping must remain live throughout the surface's use. */
int nh_surface_init(struct nh_surface *, void *, unsigned long,
    const struct fbinfo *);
int nh_surface_fill(struct nh_surface *, unsigned long, unsigned long,
    unsigned long, unsigned long, unsigned long);

#endif
