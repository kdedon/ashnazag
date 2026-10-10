#ifndef AUX_OT_PROVIDER_H
#define AUX_OT_PROVIDER_H
#include "message.h"

#define OTR_BUFFER (OTB_RHDR + OTB_MAXCTL + OTB_MAXDATA)
struct otr_provider;
struct otr_provider_ops {
	long (*read)(void *, int, unsigned char *, unsigned long);
	long (*write)(void *, int, const unsigned char *, unsigned long);
	void (*close)(void *, int);
	void (*release)(void *, struct otr_msg *);
	int (*deliver)(void *, void *, int, const unsigned char *, unsigned long,
	    const unsigned char *, unsigned long);
};
struct otr_provider {
	const struct otr_provider_ops *ops;
	void *arg, *queue;
	int fd, live, pumping, receiving, complete, type;
	unsigned long clen, dlen;
	struct otr_msg *pending;
	struct otr_tx tx;
	unsigned char send[OTR_BUFFER], ctl[OTB_MAXCTL], data[OTB_MAXDATA];
	unsigned char record[OTB_MAXREC];
};

/* Successful send transfers ownership; WAIT and BAD leave it with the caller. */
extern int otr_provider_open(struct otr_provider *,
    const struct otr_provider_ops *, void *, void *, int);
extern int otr_provider_send(struct otr_provider *, struct otr_msg *);
extern int otr_provider_pump(struct otr_provider *, unsigned int);
extern void otr_provider_flush(struct otr_provider *, int, int);
extern void otr_provider_close(struct otr_provider *);
#endif
