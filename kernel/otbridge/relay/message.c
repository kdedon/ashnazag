#include "message.h"

#if defined(__m68k__)
#include <stddef.h>
typedef char message_size[sizeof(struct otr_msg) == 28 ? 1 : -1];
typedef char datab_size[sizeof(struct otr_datab) == 28 ? 1 : -1];
typedef char type_offset[offsetof(struct otr_datab, type) == 13 ? 1 : -1];
typedef char block_offset[offsetof(struct otr_msg, block) == 20 ? 1 : -1];
#endif

int
otr_message(out, capacity, msg)
	unsigned char *out;
	unsigned long capacity;
	const struct otr_msg *msg;
{
	const struct otr_msg *p;
	unsigned long clen = 0, dlen = 0, n, at, i;
	int type, blocks = 0;

	if (!out || !msg || !msg->block)
		return OTR_BAD;
	switch (msg->block->type) {
	case OTR_M_DATA: type = OTB_R_DATA; break;
	case OTR_M_PROTO: type = OTB_R_PROTO; break;
	case OTR_M_PCPROTO: type = OTB_R_PCPROTO; break;
	default: return OTR_BAD;
	}
	for (p = msg; p; p = p->cont) {
		if (++blocks > OTR_MAXBLOCKS || !p->block || p->band ||
		    p->flags || !p->read || !p->write ||
		    (unsigned long)p->read > (unsigned long)p->write ||
		    (unsigned long)p->read < (unsigned long)p->block->base ||
		    (unsigned long)p->write > (unsigned long)p->block->limit)
			return OTR_BAD;
		n = (unsigned long)p->write - (unsigned long)p->read;
		if (p == msg && type != OTB_R_DATA) {
			if (n < 4 || n > OTB_MAXCTL)
				return OTR_BAD;
			clen = n;
		} else {
			if (p->block->type != OTR_M_DATA || n > OTB_MAXDATA - dlen)
				return OTR_BAD;
			dlen += n;
		}
	}
	if (capacity < OTB_RHDR + clen + dlen)
		return OTR_BAD;
	at = OTB_RHDR;
	for (p = msg; p; p = p->cont) {
		n = (unsigned long)p->write - (unsigned long)p->read;
		for (i = 0; i < n; i++) out[at++] = p->read[i];
	}
	return otr_pack(out, capacity, type, out + OTB_RHDR, clen,
	    out + OTB_RHDR + clen, dlen);
}
