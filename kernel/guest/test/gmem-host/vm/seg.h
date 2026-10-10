#ifndef GM_SEG_H
#define GM_SEG_H
#include "kinc.h"
typedef char *addr_t;
typedef unsigned int faultcode_t;
enum fault_type { F_INVAL, F_PROT, F_SOFTLOCK, F_SOFTUNLOCK };
enum seg_rw { S_OTHER, S_READ, S_WRITE, S_EXEC };
struct seg;
struct seg_ops {
 int (*dup)(); int (*unmap)(); void (*free)(); faultcode_t (*fault)(); int (*getprot)();
};
struct seg { addr_t s_base; u_int s_size; struct as *s_as; struct seg_ops *s_ops; void *s_data; };
#define FC_PROT 4
#define FC_MAKE_ERR(e) ((e)*256+5)
#endif
