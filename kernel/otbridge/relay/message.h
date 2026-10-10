#ifndef AUX_OT_MESSAGE_H
#define AUX_OT_MESSAGE_H

#include "record.h"

struct otr_datab {
	void *freehook;
	unsigned char *base, *limit;
	unsigned char refs, type, status, pad;
	unsigned long size;
	void *message;
	long reserved;
};

struct otr_msg {
	struct otr_msg *next, *prev, *cont;
	unsigned char *read, *write;
	struct otr_datab *block;
	unsigned char band, pad;
	unsigned short flags;
};

#define OTR_M_DATA 0
#define OTR_M_PROTO 1
#define OTR_M_PCPROTO 0x83
#define OTR_MAXBLOCKS 64

extern int otr_message(unsigned char *, unsigned long, const struct otr_msg *);

#endif
