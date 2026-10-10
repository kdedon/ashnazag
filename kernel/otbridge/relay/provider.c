#include "provider.h"

int
otr_provider_open(p, ops, arg, queue, fd)
	struct otr_provider *p;
	const struct otr_provider_ops *ops;
	void *arg, *queue;
	int fd;
{
	if (!p || p->live || p->pumping || !ops || !ops->read || !ops->write || !ops->close ||
	    !ops->release || !ops->deliver || !queue || fd < 0)
		return OTR_BAD;
	p->ops = ops; p->arg = arg; p->queue = queue; p->fd = fd;
	p->live = 1; p->pumping = p->receiving = p->complete = 0;
	p->clen = p->dlen = 0; p->pending = 0;
	return OTR_DONE;
}

void
otr_provider_close(p)
	struct otr_provider *p;
{
	struct otr_msg *pending;
	int fd;

	if (!p || !p->live)
		return;
	fd = p->fd; pending = p->pending;
	p->live = 0; p->fd = -1; p->pending = 0;
	p->receiving = p->complete = 0;
	(*p->ops->close)(p->arg, fd);
	if (pending) (*p->ops->release)(p->arg, pending);
}

int
otr_provider_send(p, message)
	struct otr_provider *p;
	struct otr_msg *message;
{
	int n;

	if (!p || !p->live || !message)
		return OTR_BAD;
	if (p->pending)
		return OTR_WAIT;
	n = otr_message(p->send, sizeof(p->send), message);
	if (n < 0 || otr_tx_start(&p->tx, p->send, (unsigned long)n) < 0)
		return OTR_BAD;
	p->pending = message;
	return OTR_DONE;
}

static long
write_record(arg, bytes, length)
	void *arg;
	const unsigned char *bytes;
	unsigned long length;
{
	struct otr_provider *p = arg;
	return (*p->ops->write)(p->arg, p->fd, bytes, length);
}

static int
receive_record(p, n)
	struct otr_provider *p;
	unsigned long n;
{
	struct otr_view v;
	unsigned long i;

	if (otr_view(p->record, n, &v) != OTR_DONE)
		return OTR_BAD;
	if (!p->receiving) {
		if (v.type != OTB_R_DATA && v.clen < 4)
			return OTR_BAD;
		p->type = v.type; p->clen = v.clen; p->dlen = 0;
		for (i = 0; i < v.clen; i++) p->ctl[i] = v.ctl[i];
	} else if (v.type != p->type || v.clen)
		return OTR_BAD;
	if (v.dlen > OTB_MAXDATA - p->dlen ||
	    (v.more && !v.dlen))
		return OTR_BAD;
	for (i = 0; i < v.dlen; i++) p->data[p->dlen + i] = v.data[i];
	p->dlen += v.dlen; p->receiving = !!v.more;
	p->complete = !v.more;
	return OTR_DONE;
}

/* A bounded pass preserves stalled messages for the next readiness event. */
int
otr_provider_pump(p, budget)
	struct otr_provider *p;
	unsigned int budget;
{
	int r, progressed = 0;
	long n;
	struct otr_msg *done;

	if (!p || !p->live || !budget)
		return OTR_BAD;
	if (p->pumping)
		return OTR_WAIT;
	p->pumping = 1;
	while (budget-- && p->live) {
		if (p->pending) {
			r = otr_tx_step(&p->tx, write_record, p);
			if (!p->live) break;
			if (r < 0) goto bad;
			if (r == OTR_DONE) {
				done = p->pending; p->pending = 0;
				(*p->ops->release)(p->arg, done);
				progressed = 1;
			}
			if (!p->live) break;
		}
		if (p->complete) {
			r = (*p->ops->deliver)(p->arg, p->queue, p->type,
			    p->ctl, p->clen, p->data, p->dlen);
			if (r < 0) goto bad;
			if (!p->live) break;
			if (r == OTR_WAIT) break;
			if (r != OTR_DONE) goto bad;
			p->complete = 0; progressed = 1;
		}
		n = (*p->ops->read)(p->arg, p->fd, p->record, sizeof(p->record));
		if (!p->live) break;
		if (n < 0 || (unsigned long)n > sizeof(p->record)) goto bad;
		if (!n) break;
		if (receive_record(p, (unsigned long)n) < 0) goto bad;
		progressed = 1;
	}
	p->pumping = 0;
	return progressed ? OTR_DONE : OTR_WAIT;
bad:
	otr_provider_close(p); p->pumping = 0;
	return OTR_BAD;
}

void
otr_provider_flush(p, read, write)
	struct otr_provider *p;
	int read, write;
{
	struct otr_msg *mp;
	if (!p || !p->live) return;
	if (read) { p->receiving = p->complete = 0; p->clen = p->dlen = 0; }
	if (write && p->pending) {
		mp = p->pending; p->pending = 0;
		(*p->ops->release)(p->arg, mp);
	}
}
