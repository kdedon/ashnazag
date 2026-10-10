#include "bootstrap.h"
#include <stddef.h>

#if defined(__m68k__)
typedef char otr_qinit_size[(sizeof(struct otr_qinit) == 28) ? 1 : -1];
typedef char otr_streamtab_size[(sizeof(struct otr_streamtab) == 16) ? 1 : -1];
typedef char otr_install_size[(sizeof(struct otr_install) == 24) ? 1 : -1];
typedef char otr_install_flags[(offsetof(struct otr_install, flags) == 4) ? 1 : -1];
typedef char otr_qinit_open[(offsetof(struct otr_qinit, open) == 8) ? 1 : -1];
#endif

int
otr_boot_close(b)
	struct otr_boot *b;
{
	int fd, session;

	if (!b || !b->live) return OTR_DONE;
	if (b->children) return OTR_WAIT;
	fd = b->fd; session = b->session;
	b->live = 0; b->fd = b->session = -1; b->id = 0;
	if (fd >= 0) (*b->ops->close)(b->arg, fd);
	if (session >= 0) (*b->ops->close)(b->arg, session);
	if (b->owner) { b->owner->children--; b->owner = 0; }
	return OTR_DONE;
}

int
otr_boot_open(b, ops, arg, protocol, cookie)
	struct otr_boot *b;
	const struct otr_boot_ops *ops;
	void *arg;
	int protocol;
	unsigned long cookie;
{
	long id;
	if (!b || b->live || !ops || !ops->session || !ops->session_id || !ops->endpoint ||
	    !ops->push || !ops->attach || !ops->close || !cookie ||
	    (protocol != OTR_TCP && protocol != OTR_UDP)) return OTR_BAD;
	b->owner = 0; b->children = 0;
	b->live = 1; b->ops = ops; b->arg = arg; b->fd = b->session = -1; b->id = 0;
	b->session = (*ops->session)(arg);
	if (b->session < 0) goto bad;
	id = (*ops->session_id)(arg, b->session);
	if (id <= 0) goto bad;
	b->id = id;
	b->fd = (*ops->endpoint)(arg, protocol);
	if (b->fd < 0 || (*ops->push)(arg, b->fd) < 0 ||
	    (*ops->attach)(arg, b->fd, b->id, cookie, protocol) < 0) goto bad;
	return OTR_DONE;
bad:
	otr_boot_close(b);
	return OTR_BAD;
}

int
otr_install_contract(out, table, protocol)
	struct otr_install *out;
	struct otr_streamtab *table;
	int protocol;
{
	if (!out || !table || !table->read || !table->write || table->muxread ||
	    table->muxwrite || !table->read->put || !table->write->put ||
	    !table->read->open || !table->read->close || !table->read->info ||
	    !table->write->info || (protocol != OTR_TCP && protocol != OTR_UDP))
		return OTR_BAD;
	out->table = table; out->flags = 0x1001UL;
	out->sync = protocol == OTR_TCP ? 3UL : 1UL;
	out->buddy = 0; out->loads = out->references = 0;
	return OTR_DONE;
}

int
otr_boot_join(b, owner, protocol, cookie)
	struct otr_boot *b, *owner;
	int protocol;
	unsigned long cookie;
{
	if (!b || b->live || !owner || !owner->live || owner->owner ||
	    !cookie || owner->children == (unsigned int)-1 ||
	    (protocol != OTR_TCP && protocol != OTR_UDP)) return OTR_BAD;
	b->live = 1; b->ops = owner->ops; b->arg = owner->arg;
	b->session = b->fd = -1; b->id = owner->id;
	b->owner = owner; b->children = 0; owner->children++;
	b->fd = (*b->ops->endpoint)(b->arg, protocol);
	if (b->fd < 0 || (*b->ops->push)(b->arg, b->fd) < 0 ||
	    (*b->ops->attach)(b->arg, b->fd, b->id, cookie, protocol) < 0) {
		otr_boot_close(b);
		return OTR_BAD;
	}
	return OTR_DONE;
}

int
otr_boot_session(b, ops, arg)
	struct otr_boot *b;
	const struct otr_boot_ops *ops;
	void *arg;
{
	long id;
	if (!b || b->live || !ops || !ops->session || !ops->session_id ||
	    !ops->endpoint || !ops->push || !ops->attach || !ops->close) return OTR_BAD;
	b->owner = 0; b->children = 0;
	b->live = 1; b->ops = ops; b->arg = arg;
	b->fd = b->session = -1; b->id = 0;
	b->session = (*ops->session)(arg);
	if (b->session < 0) goto bad;
	id = (*ops->session_id)(arg, b->session);
	if (id <= 0) goto bad;
	b->id = id;
	return OTR_DONE;
bad:
	otr_boot_close(b);
	return OTR_BAD;
}
