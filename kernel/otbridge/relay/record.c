#include "record.h"

static unsigned long
get32(p)
	const unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

static void
put32(p, n)
	unsigned char *p;
	unsigned long n;
{
	p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}

static int
valid(type, clen, dlen)
	int type;
	unsigned long clen, dlen;
{
	return (type == OTB_R_DATA || type == OTB_R_PROTO ||
	    type == OTB_R_PCPROTO) && clen <= OTB_MAXCTL &&
	    dlen <= OTB_MAXDATA && (type != OTB_R_DATA || clen == 0);
}

int
otr_pack(out, capacity, type, ctl, clen, data, dlen)
	unsigned char *out;
	unsigned long capacity, clen, dlen;
	int type;
	const unsigned char *ctl, *data;
{
	unsigned long i, length;

	if (!out || !valid(type, clen, dlen) || (clen && !ctl) ||
	    (dlen && !data) || (type != OTB_R_DATA && clen < 4))
		return OTR_BAD;
	length = OTB_RHDR + clen + dlen;
	if (capacity < length)
		return OTR_BAD;
	out[0] = type; out[1] = 0;
	out[2] = clen >> 8; out[3] = clen;
	put32(out + 4, dlen);
	for (i = 0; i < clen; i++) out[OTB_RHDR + i] = ctl[i];
	for (i = 0; i < dlen; i++) out[OTB_RHDR + clen + i] = data[i];
	return (int)length;
}

int
otr_view(bytes, length, view)
	const unsigned char *bytes;
	unsigned long length;
	struct otr_view *view;
{
	unsigned long clen, dlen;

	if (!bytes || !view || length < OTB_RHDR)
		return OTR_BAD;
	clen = (unsigned long)bytes[2] << 8 | bytes[3];
	dlen = get32(bytes + 4);
	if (!valid(bytes[0], clen, dlen) || (bytes[1] & ~OTB_RF_MORE) ||
	    OTB_RHDR + clen + dlen != length)
		return OTR_BAD;
	view->type = bytes[0]; view->more = bytes[1] & OTB_RF_MORE;
	view->clen = clen; view->dlen = dlen;
	view->ctl = bytes + OTB_RHDR;
	view->data = view->ctl + clen;
	return OTR_DONE;
}

int
otr_tx_start(tx, bytes, length)
	struct otr_tx *tx;
	const unsigned char *bytes;
	unsigned long length;
{
	struct otr_view view;

	if (!tx || otr_view(bytes, length, &view) != OTR_DONE || view.more ||
	    (view.type != OTB_R_DATA && view.clen < 4))
		return OTR_BAD;
	tx->bytes = bytes; tx->length = length; tx->sent = 0;
	return OTR_DONE;
}

/* Each call makes one nonblocking write; the caller retains the record. */
int
otr_tx_step(tx, writefn, arg)
	struct otr_tx *tx;
	long (*writefn)(void *, const unsigned char *, unsigned long);
	void *arg;
{
	long n;
	unsigned long left;

	if (!tx || !writefn || !tx->bytes || tx->sent > tx->length)
		return OTR_BAD;
	left = tx->length - tx->sent;
	if (!left)
		return OTR_DONE;
	n = (*writefn)(arg, tx->bytes + tx->sent, left);
	if (n < 0 || (unsigned long)n > left)
		return OTR_BAD;
	tx->sent += n;
	return tx->sent == tx->length ? OTR_DONE : OTR_WAIT;
}
