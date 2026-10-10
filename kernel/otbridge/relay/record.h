#ifndef AUX_OT_RECORD_H
#define AUX_OT_RECORD_H

#include "../otwire.h"

struct otr_view {
	int type, more;
	const unsigned char *ctl, *data;
	unsigned long clen, dlen;
};

struct otr_tx {
	const unsigned char *bytes;
	unsigned long length, sent;
};

#define OTR_BAD -1
#define OTR_WAIT 0
#define OTR_DONE 1

extern int otr_pack(unsigned char *, unsigned long, int,
    const unsigned char *, unsigned long, const unsigned char *, unsigned long);
extern int otr_view(const unsigned char *, unsigned long, struct otr_view *);
extern int otr_tx_start(struct otr_tx *, const unsigned char *, unsigned long);
extern int otr_tx_step(struct otr_tx *, long (*)(void *, const unsigned char *,
    unsigned long), void *);

#endif
