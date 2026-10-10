#ifndef AUX_OT_HOST_H
#define AUX_OT_HOST_H
#include "bootstrap.h"
#define OTR_HOST_EOF (-32767L)
extern long otr_host_step, otr_host_result;
extern int otr_host_wait(int, int, unsigned long);
extern int otr_host_flush(int, int);
extern const struct otr_boot_ops otr_host_boot;
extern long otr_host_read(void *, int, unsigned char *, unsigned long);
extern long otr_host_write(void *, int, const unsigned char *, unsigned long);
extern void otr_host_close(void *, int);
#endif
