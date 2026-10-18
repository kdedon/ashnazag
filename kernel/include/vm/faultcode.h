/* Segment fault results: a code in the low byte, an errno above it. */
#ifndef _VM_FAULTCODE_H
#define _VM_FAULTCODE_H

#define FC_HWERR	0x1
#define FC_ALIGN	0x2
#define FC_NOMAP	0x3
#define FC_PROT		0x4
#define FC_OBJERR	0x5
#define FC_MAKE_ERR(e)	(((e) << 8) | FC_OBJERR)
#define FC_CODE(fc)	((fc) & 0xff)
#define FC_ERRNO(fc)	((unsigned)(fc) >> 8)

typedef int faultcode_t;

#endif
