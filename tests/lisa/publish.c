#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "lisa_host.h"
#include "lisa_wire.h"
static int remaining=1;
void nh_display_init(struct nh_display *d) { memset(d,0,sizeof *d);d->framebuffer=d->keyboard=d->mouse=-1; }
int nh_display_open(struct nh_display *d,const char *a,const char *b,int c) { (void)d;(void)a;(void)b;(void)c;return -1; }
int nh_display_close(struct nh_display *d) { (void)d;return 0; }
int lisa_audio_stop(struct lisa_host *h) { (void)h;return 0; }
int nh_input(int fd,struct inev *e) { (void)fd;if(!remaining)return 0;remaining--;memset(e,0,sizeof *e);e->ie_type=IE_KEY;e->ie_unit=3;e->ie_code=57;e->ie_value=1;e->ie_sec=12345;e->ie_usec=678;return 1; }
int test_ioctl(int fd,unsigned long request,...) { va_list a;void *p;(void)fd;va_start(a,request);p=va_arg(a,void *);va_end(a);if(request==EVIOCGKEYS){memset(p,0,16);((unsigned char*)p)[57/8]|=1<<(57%8);return 0;}return -1; }
int main(void) { struct lisa_host h;unsigned char *surface=calloc(0x20000,1),*k=surface+LISA_KEYBOARD,*p=surface+LISA_INPUT,*e=p+16;lisa_host_init(&h);h.keyboard_info.ei_kset=EVK_ADB;h.keyboard_info.ei_id=17;h.keyboard_info.ei_flags=EVF_CAPSLATCH;lisa_put32(k,0x4c4b4244);lisa_put32(k+4,1);lisa_put32(p,0x4c494e50);lisa_put32(p+4,1);assert(lisa_host_publish(&h,surface)==1);assert(lisa_get32(k+8)==1&&lisa_get32(k+12)==17&&k[20+57/8]==2);assert(lisa_get32(p+12)==1&&e[0]==IE_KEY&&e[1]==3&&e[2]==0&&e[3]==57&&lisa_get32(e+4)==1&&lisa_get32(e+8)==12345&&lisa_get32(e+12)==678);lisa_put32(p+12,64);remaining=1;assert(lisa_host_publish(&h,surface)==0&&remaining==1);lisa_put32(p+12,65);assert(lisa_host_publish(&h,surface)==-1&&remaining==1);free(surface);return 0; }
