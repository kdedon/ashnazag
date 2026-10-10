#ifndef AUX_OT_BOOTSTRAP_H
#define AUX_OT_BOOTSTRAP_H
#include "provider.h"

#define OTR_TCP 1
#define OTR_UDP 2
struct otr_boot_ops {
	int (*session)(void *);
	long (*session_id)(void *, int);
	int (*endpoint)(void *, int);
	int (*push)(void *, int);
	int (*attach)(void *, int, unsigned long, unsigned long, int);
	void (*close)(void *, int);
};
struct otr_boot {
	const struct otr_boot_ops *ops;
	void *arg;
	int fd, session, live;
	unsigned long id;
	struct otr_boot *owner;
	unsigned int children;
};

#if defined(__m68k__)
#pragma pack(2)
#endif
struct otr_qinit {
	long (*put)(void *, struct otr_msg *);
	long (*service)(void *);
	long (*open)(void *, void *, long, long, void *);
	long (*close)(void *, long, void *);
	void *admin, *info, *stats;
};
struct otr_streamtab {
	struct otr_qinit *read, *write, *muxread, *muxwrite;
};
struct otr_install {
	struct otr_streamtab *table;
	unsigned long flags, sync;
	void *buddy;
	unsigned long loads, references;
};
#if defined(__m68k__)
#pragma pack()
#endif

/* Adapter operations return negative errors and open nonblocking descriptors. */
extern int otr_boot_session(struct otr_boot *, const struct otr_boot_ops *, void *);
extern int otr_boot_open(struct otr_boot *, const struct otr_boot_ops *,
    void *, int, unsigned long);
extern int otr_boot_join(struct otr_boot *, struct otr_boot *, int, unsigned long);
extern int otr_boot_close(struct otr_boot *);
extern int otr_install_contract(struct otr_install *, struct otr_streamtab *, int);
#endif
