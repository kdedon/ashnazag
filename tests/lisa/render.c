#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lisa_host.h"
#include "lisa_wire.h"
void nh_display_init(struct nh_display *d) { memset(d,0,sizeof *d);d->framebuffer=d->keyboard=d->mouse=-1;d->mapping=(void *)-1; }
int nh_display_open(struct nh_display *d,const char *p,const char *n,int f) { (void)d;(void)p;(void)n;(void)f;return -1; }
int nh_display_close(struct nh_display *d) { (void)d;return 0; }
int nh_input(int fd,struct inev *e) { (void)fd;(void)e;return 0; }
int lisa_audio_stop(struct lisa_host *h) { (void)h;return 0; }
static unsigned long getpixel(struct nh_surface *s,unsigned long x,unsigned long y) { unsigned char *p=s->pixels+y*s->stride;unsigned long n,v=0;if(s->depth<8)return (p[x*s->depth/8]>>(8-s->depth-x*s->depth%8))&((1UL<<s->depth)-1);p+=x*(s->depth/8);for(n=0;n<s->depth/8;n++)v=(v<<8)|p[n];return v; }
int main(void) { struct lisa_host h;struct fbinfo f;unsigned char map[256],bits[2]={0x81,0x42};unsigned int depths[]={1,2,4,8,16,24,32},n;unsigned char *surface=calloc(0x20000,1),*p=calloc(800*600,1),*c=surface+LISA_CURSOR;for(n=0;n<7;n++){memset(&f,0,sizeof f);memset(map,0xcc,sizeof map);f.fi_layout=FBL_PACKED;f.fi_width=8;f.fi_height=2;f.fi_depth=depths[n];f.fi_rowbytes=40;f.fi_offset=5;f.fi_size=85;lisa_host_init(&h);assert(!nh_surface_init(&h.surface,map,sizeof map,&f));h.black=1;assert(!lisa_host_present(&h,bits,8,2,1));assert(getpixel(&h.surface,0,0)==1&&getpixel(&h.surface,1,0)==0&&getpixel(&h.surface,7,0)==1&&getpixel(&h.surface,1,1)==1);assert(map[4]==0xcc&&map[84]==0xcc);lisa_host_close(&h);}lisa_host_init(&h);h.surface.pixels=p;h.surface.width=800;h.surface.height=600;h.surface.stride=800;h.surface.depth=8;h.black=1;memset(surface+LISA_BITMAP,255,32760);lisa_put32(c,1);lisa_put32(c+4,1);lisa_put32(c+28,2);c[32]=0x80;c[96]=0xc0;assert(!lisa_host_render(&h,surface));assert(h.view_height==404&&h.view_y==98);assert(p[98*800]==1&&p[98*800+2]==0&&p[98*800+3]==1);assert(surface[LISA_BITMAP]==255);lisa_put32(c+8,1);assert(!lisa_host_render(&h,surface));assert(p[98*800]==0&&p[98*800+2]==1);free(p);free(surface);lisa_host_close(&h);return 0; }
