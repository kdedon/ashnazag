/*
 * The kernel's root filesystem over host calls.  These routines run inside
 * the guest kernel as a filesystem type: each vnode and vfs operation is
 * one request in the mailbox (hfswire.h).  startnext copies the code to
 * HF_CODE, fills the context with the kernel's own routines found by their
 * symbols, calls hf_init, and points a filesystem-switch entry at hf_vfsops.
 */
#include "hfswire.h"

struct hnode {
	unsigned char vn[64];		/* the kernel's vnode, first */
	unsigned long id;
	struct hnode *next;
	unsigned long dcur_off, dcur_idx;
};

struct hctx {
	long (*kalloc)();
	long (*vm_info_init)();
	long (*uiomove)();
	long (*vfs_add)();
	long (*vfs_unlock)();
	long (*bcopy)();
	long (*specvp)();
	unsigned long *active_u;
	long (*hcall)();
	unsigned long vfsops[8], vnodeops[36];
	struct hnode *hash[64], *freelist;
	unsigned long vfsp;
	unsigned long rootnode;
	unsigned char *spare;		/* free copy buffers, linked through their first long */
};

#define C ((struct hctx *)HF_CTX)
#define B ((struct hf_box *)HF_BASE)
#define W16(p, o) (*(unsigned short *)((p) + (o)))
#define W32(p, o) (*(unsigned long *)((p) + (o)))

/* vnode fields */
#define V_FLAG 4
#define V_COUNT 6
#define V_OP 28
#define V_VFSP 36
#define V_TYPE 40
#define V_RDEV 44
#define V_DATA 46
#define VROOT 1

static long
host(op)
	unsigned long op;
{
	struct hf_box *b = B;
	long spin;

	b->magic = HF_MAGIC;
	b->op = op;
	b->state = HF_REQUEST;
	C->hcall(HC_HFS);
	for (spin = 0; b->state != HF_REPLY && spin < 2000000000L; spin++)
		;
	b->state = HF_IDLE;
	return b->error;
}

static void
clear(p, n)
	unsigned char *p;
	unsigned long n;
{
	while (n--)
		*p++ = 0;
}

static void
setsize(vp, size)
	unsigned char *vp;
	unsigned long size;
{
	unsigned long *vi = (unsigned long *)W32(vp, 0);

	if (vi)
		vi[5] = size;
}

/* the vnode for node id, held; the box holds its attributes */
static long
getnode(id, vfsp, vpp)
	unsigned long id, vfsp;
	unsigned char **vpp;
{
	struct hf_box *b = B;
	struct hnode *n;
	unsigned long vi;
	unsigned char *vp;

	for (n = C->hash[id & 63]; n; n = n->next)
		if (n->id == id)
			break;
	if (!n) {
		vi = 0;
		if ((n = C->freelist) != 0) {
			C->freelist = n->next;
			vi = W32(n->vn, 0);
		} else if ((n = (struct hnode *)C->kalloc(sizeof *n)) == 0)
			return 12;
		vp = n->vn;
		clear((unsigned char *)n, sizeof *n);
		W32(vp, 0) = vi;
		C->vm_info_init(vp);
		if ((vi = W32(vp, 0)) != 0)
			*(unsigned long *)vi = 0;
		n->id = id;
		n->next = C->hash[id & 63];
		C->hash[id & 63] = n;
		W16(vp, V_COUNT) = 0;
		W32(vp, V_OP) = (unsigned long)C->vnodeops;
		W32(vp, V_VFSP) = vfsp;
		W32(vp, V_DATA) = id;
	}
	vp = n->vn;
	W32(vp, V_TYPE) = b->type;
	W16(vp, V_RDEV) = b->rdev;
	setsize(vp, b->size);
	W16(vp, V_COUNT) = W16(vp, V_COUNT) + 1;
	*vpp = vp;
	return 0;
}

static void
fill(vap)
	unsigned char *vap;
{
	struct hf_box *b = B;

	W32(vap, 0) = b->type;
	W16(vap, 4) = b->amode;
	W16(vap, 6) = b->auid;
	W16(vap, 8) = b->agid;
	W32(vap, 10) = 1;
	W32(vap, 14) = b->ino;
	W16(vap, 18) = b->nlink;
	W32(vap, 20) = b->size;
	W32(vap, 24) = b->bsize;
	W32(vap, 28) = b->atime;
	W32(vap, 32) = 0;
	W32(vap, 36) = b->mtime;
	W32(vap, 40) = 0;
	W32(vap, 44) = b->ctime;
	W32(vap, 48) = 0;
	W16(vap, 52) = b->rdev;
	W32(vap, 54) = b->blocks;
}

static unsigned char *
cred()
{
	return (unsigned char *)W32((unsigned char *)*C->active_u, 26);
}

static void
setname(dst, src)
	char *dst, *src;
{
	int i;

	for (i = 0; i < HF_NAME - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

#define NODE(vp) W32((unsigned char *)(vp), V_DATA)

/* a new or found node from a request that returned node2 */
static long
child(dvp, nm, vpp)
	unsigned char *dvp;
	char *nm;
	unsigned char **vpp;
{
	long e;
	unsigned char *vp, *sp;
	unsigned long type, rdev;

	if ((e = B->error) != 0)
		return e;
	type = B->type;
	rdev = B->rdev;
	if ((e = getnode(B->node2, W32(dvp, V_VFSP), &vp)) != 0)
		return e;
	if (type == HF_VCHR || type == HF_VBLK) {
		/* devices are the kernel's special vnodes; they hold the node */
		sp = (unsigned char *)C->specvp(vp, rdev, type);
		W16(vp, V_COUNT) = W16(vp, V_COUNT) - 1;
		vp = sp;
	}
	*vpp = vp;
	return 0;
}

static void hf_keys();
void snd_init();

/* the filesystem type's operations */
static long
hf_mountroot(vfsp, vpp, name)
	unsigned char *vfsp;
	unsigned char **vpp;
	char *name;
{
	unsigned char *vp;
	long e;

	W32(vfsp, 4) = (unsigned long)C->vfsops;
	W32(vfsp, 16) = HF_DATA;
	W32(vfsp, 294) = 0;
	C->vfsp = (unsigned long)vfsp;
	if ((e = host(HF_ROOT)) != 0)
		return e;
	if ((e = getnode(1, (unsigned long)vfsp, &vp)) != 0)
		return e;
	W16(vp, V_FLAG) |= VROOT;
	C->rootnode = (unsigned long)vp;
	if ((e = C->vfs_add(0, vfsp, 0)) != 0)
		return e;
	C->vfs_unlock(vfsp);
	if (((struct hf_keys *)HF_KEYS)->input)
		hf_keys();
	*vpp = vp;
	return 0;
}

/* the frame buffer driver's retrace interrupt enable and disable */
static void
hf_retrace()
{
	struct hf_keys *k = (struct hf_keys *)HF_KEYS;

	if (!(k->running = k->retrace != 0))
		return;
	k->retrace(k->rarg);
	k->timeout(hf_retrace, 0, 1);
}

static long
hf_vidon(fn, arg)
	long (*fn)();
	unsigned long arg;
{
	struct hf_keys *k = (struct hf_keys *)HF_KEYS;

	k->rarg = arg;
	k->retrace = fn;
	if (!k->running) {
		k->running = 1;
		k->timeout(hf_retrace, 0, 1);
	}
	return 0;
}

static long
hf_vidoff()
{
	((struct hf_keys *)HF_KEYS)->retrace = 0;
	return 0;
}

/* hand typed characters to the console's input */
static void
hf_keys()
{
	struct hf_keys *k = (struct hf_keys *)HF_KEYS;
	int events = k->open && *(long *)k->open;
	unsigned long w;

	while (k->tail != k->head) {
		if (!events)
			k->input(k->buf[k->tail & 255]);
		k->tail++;
	}
	while (k->etail != k->ehead) {
		w = k->ev[k->etail & 63];
		k->etail++;
		if (!events)
			continue;
		if (w & HF_MOUSE) {
			w &= 0xffff;
			k->mintr(&w);
		} else
			k->kintr(&w);
	}
	k->timeout(hf_keys, 0, k->ticks);
}

static long
hf_root(vfsp, vpp)
	unsigned char *vfsp;
	unsigned char **vpp;
{
	unsigned char *vp = (unsigned char *)C->rootnode;

	W16(vp, V_COUNT) = W16(vp, V_COUNT) + 1;
	*vpp = vp;
	return 0;
}

static long hf_zero() { return 0; }
static long hf_inval() { return 22; }
static long hf_notty() { return 25; }

static long
hf_getattr(vp, vap)
	unsigned char *vp, *vap;
{
	long e;

	B->node = NODE(vp);
	if ((e = host(HF_GETATTR)) != 0)
		return e;
	fill(vap);
	setsize(vp, B->size);
	return 0;
}

static long
hf_setattr(vp, vap)
	unsigned char *vp, *vap;
{
	struct hf_box *b = B;
	unsigned long fl = 0;
	long e;

	b->node = NODE(vp);
	b->mode = W16(vap, 4);
	b->uid = W16(vap, 6);
	b->gid = W16(vap, 8);
	b->length = W32(vap, 20);
	b->offset = W32(vap, 36);
	if (W16(vap, 4) != 0xffff)
		fl |= HF_SET_MODE;
	if (W16(vap, 6) != 0xffff || W16(vap, 8) != 0xffff)
		fl |= HF_SET_OWNER;
	if (W32(vap, 20) != 0xffffffffUL)
		fl |= HF_SET_SIZE;
	if (W32(vap, 36) != 0xffffffffUL)
		fl |= HF_SET_TIMES;
	b->flags = fl;
	if ((e = host(HF_SETATTR)) != 0)
		return e;
	setsize(vp, b->size);
	return 0;
}

static long
hf_access(vp, mode)
	unsigned char *vp;
	long mode;
{
	struct hf_box *b = B;
	unsigned char *cr = cred();
	unsigned long uid = W16(cr, 2), gid = W16(cr, 4), m = b->amode;
	long e, want = (mode >> 6) & 7;

	b->node = NODE(vp);
	if ((e = host(HF_GETATTR)) != 0)
		return e;
	m = b->amode;
	if (uid == 0)
		return (want & 1) && b->type != HF_VDIR && !(m & 0111) ? 13 : 0;
	if (uid == b->auid)
		m >>= 6;
	else if (gid == b->agid)
		m >>= 3;
	return (m & want) == want ? 0 : 13;
}

static long
hf_lookup(dvp, nm, vpp)
	unsigned char *dvp, **vpp;
	char *nm;
{
	B->node = NODE(dvp);
	setname(B->name, nm);
	host(HF_LOOKUP);
	return child(dvp, nm, vpp);
}

/*
 * Copies to and from the caller go through a buffer of its own: a page
 * fault there pages in through the mailbox and would overwrite it.
 */
static unsigned char *
getbuf()
{
	unsigned char *p = C->spare;

	if (p)
		C->spare = (unsigned char *)W32(p, 0);
	else
		p = (unsigned char *)C->kalloc(HF_DATA);
	return p;
}

static void
putbuf(p)
	unsigned char *p;
{
	W32(p, 0) = (unsigned long)C->spare;
	C->spare = p;
}

static long
hf_rdwr(vp, uio, rw, ioflag)
	unsigned char *vp, *uio;
	long rw, ioflag;
{
	struct hf_box *b = B;
	unsigned char *buf;
	unsigned long off;
	long e = 0, got, n;

	if ((buf = getbuf()) == 0)
		return 12;
	while (W32(uio, 18) > 0) {
		n = W32(uio, 18) > HF_DATA ? HF_DATA : W32(uio, 18);
		off = W32(uio, 8);
		if (rw == 1) {
			if ((e = C->uiomove(buf, n, 1, uio)) != 0)
				break;
			C->bcopy(buf, b->data, n);
			b->node = NODE(vp);
			b->offset = off;
			b->length = n;
			if ((e = host(HF_WRITE)) != 0)
				break;
			continue;
		}
		b->node = NODE(vp);
		b->offset = off;
		b->length = n;
		if ((e = host(HF_READ)) != 0)
			break;
		got = b->result;
		if (got <= 0)
			break;
		C->bcopy(b->data, buf, got);
		if ((e = C->uiomove(buf, got, 0, uio)) != 0 || got < n)
			break;
	}
	putbuf(buf);
	return e;
}

/* a page of the file into the page frame m names; the part past the end is zero */
static long
hf_pagein(vp, m, off)
	unsigned char *vp, *m;
	unsigned long off;
{
	struct hf_box *b = B;
	unsigned char *pg = (unsigned char *)W32(m, 34);
	long e, got, i;

	b->node = NODE(vp);
	b->offset = off;
	b->length = HF_DATA;
	if ((e = host(HF_READ)) != 0)
		return e;
	got = b->result < 0 ? 0 : b->result;
	C->bcopy(b->data, pg, got);
	for (i = got; i < HF_DATA; i++)
		pg[i] = 0;
	return 0;
}

static long
hf_pageout(vp, pg, len, off)
	unsigned char *vp, *pg;
	unsigned long len, off;
{
	struct hf_box *b = B;

	if (len > HF_DATA)
		len = HF_DATA;
	C->bcopy(pg, b->data, len);
	b->node = NODE(vp);
	b->offset = off;
	b->length = len;
	return host(HF_WRITE);
}

static long
hf_readdir(vp, uio)
	unsigned char *vp, *uio;
{
	struct hf_box *b = B;
	struct hnode *n = (struct hnode *)vp;
	unsigned char blk[1024];
	unsigned long off, idx, i, last, rl, used;
	long e;

	if (W32(uio, 8) & 1023)
		return 22;
	while (W32(uio, 18) >= 1024) {
		off = W32(uio, 8);
		if (off == n->dcur_off)
			idx = n->dcur_idx;
		else if (off == 0)
			idx = 0;
		else
			return 22;
		b->node = NODE(vp);
		b->offset = idx;
		b->length = 1024;
		if ((e = host(HF_READDIR)) != 0)
			return e;
		used = b->result;
		if (used == 0)
			return 0;
		C->bcopy(b->data, blk, used);
		for (i = 0, last = 0; i < used; i += rl) {
			last = i;
			rl = W16(blk, i + 4);
			if (rl < 8)
				return 5;
		}
		W16(blk, last + 4) = 1024 - last;
		clear(blk + used, 1024 - used);
		n->dcur_idx = b->offset;
		n->dcur_off = off + 1024;
		if ((e = C->uiomove(blk, 1024, 0, uio)) != 0)
			return e;
		if (!b->flags)
			n->dcur_idx = 0xffffffffUL;
	}
	return 0;
}

static long
hf_create(dvp, nm, vap, excl, mode, vpp)
	unsigned char *dvp, *vap, **vpp;
	char *nm;
	long excl, mode;
{
	struct hf_box *b = B;
	unsigned char *cr = cred();
	long e;

	unsigned long type = W32(vap, 0);

	if (type != HF_VREG && type != HF_VCHR && type != HF_VBLK && type != HF_VSOCK)
		return 22;
	b->node = NODE(dvp);
	setname(b->name, nm);
	b->mode = W16(vap, 4) != 0xffff ? W16(vap, 4) & 07777 : mode & 07777;
	b->uid = W16(cr, 2);
	b->gid = W16(cr, 4);
	e = host(HF_LOOKUP);
	if (e == 0 && !excl && type == HF_VREG) {
		if ((e = child(dvp, nm, vpp)) != 0)
			return e;
		if (W32(vap, 20) == 0) {
			b->node = NODE(*vpp);
			b->flags = HF_SET_SIZE;
			b->length = 0;
			host(HF_SETATTR);
			setsize(*vpp, 0);
		}
		return 0;
	}
	if (e == 0)
		return 17;
	b->node = NODE(dvp);
	setname(b->name, nm);
	b->mode = W16(vap, 4) != 0xffff ? W16(vap, 4) & 07777 : mode & 07777;
	b->uid = W16(cr, 2);
	b->gid = W16(cr, 4);
	b->flags = type;
	b->rdev = W16(vap, 52);
	host(HF_CREATE);
	return child(dvp, nm, vpp);
}

static long
hf_name(op, dvp, nm)
	unsigned long op;
	unsigned char *dvp;
	char *nm;
{
	B->node = NODE(dvp);
	setname(B->name, nm);
	return host(op);
}

static long hf_remove(dvp, nm) unsigned char *dvp; char *nm; { return hf_name(HF_REMOVE, dvp, nm); }
static long hf_rmdir(dvp, nm) unsigned char *dvp; char *nm; { return hf_name(HF_RMDIR, dvp, nm); }

static long
hf_link(vp, tdvp, tnm)
	unsigned char *vp, *tdvp;
	char *tnm;
{
	B->node = NODE(vp);
	B->node2 = NODE(tdvp);
	setname(B->name, tnm);
	return host(HF_LINK);
}

static long
hf_rename(sdvp, snm, tdvp, tnm)
	unsigned char *sdvp, *tdvp;
	char *snm, *tnm;
{
	B->node = NODE(sdvp);
	B->node2 = NODE(tdvp);
	setname(B->name, snm);
	setname(B->name2, tnm);
	return host(HF_RENAME);
}

static long
hf_mkdir(dvp, nm, vap, vpp)
	unsigned char *dvp, *vap, **vpp;
	char *nm;
{
	unsigned char *cr = cred();

	B->node = NODE(dvp);
	setname(B->name, nm);
	B->mode = W16(vap, 4) & 07777;
	B->uid = W16(cr, 2);
	B->gid = W16(cr, 4);
	host(HF_MKDIR);
	return child(dvp, nm, vpp);
}

static long
hf_symlink(dvp, nm, vap, tnm)
	unsigned char *dvp, *vap;
	char *nm, *tnm;
{
	struct hf_box *b = B;
	unsigned char *cr = cred();
	long i;

	b->node = NODE(dvp);
	setname(b->name, nm);
	for (i = 0; i < HF_DATA - 1 && tnm[i]; i++)
		b->data[i] = tnm[i];
	b->length = i;
	b->mode = W16(vap, 4) & 07777;
	b->uid = W16(cr, 2);
	b->gid = W16(cr, 4);
	return host(HF_SYMLINK);
}

static long
hf_readlink(vp, uio)
	unsigned char *vp, *uio;
{
	unsigned char *buf;
	long e, n;

	B->node = NODE(vp);
	if ((e = host(HF_READLINK)) != 0)
		return e;
	if ((buf = getbuf()) == 0)
		return 12;
	n = B->result;
	C->bcopy(B->data, buf, n);
	e = C->uiomove(buf, n, 0, uio);
	putbuf(buf);
	return e;
}

static long
hf_inactive(vp)
	unsigned char *vp;
{
	struct hnode *n = (struct hnode *)vp, **pp;

	B->node = n->id;
	host(HF_INACTIVE);
	for (pp = &C->hash[n->id & 63]; *pp && *pp != n; pp = &(*pp)->next)
		;
	if (*pp)
		*pp = n->next;
	n->next = C->freelist;
	C->freelist = n;
	return 0;
}

static long
hf_cmp(a, b)
	unsigned char *a, *b;
{
	return a == b;
}

/* the kernel's statfs: type, block size, blocks, free, available, files, free files */
static long
hf_statfs(vfsp, sbp)
	unsigned char *vfsp, *sbp;
{
	struct hf_box *b = B;

	if (host(HF_STATFS))
		return 5;
	W32(sbp, 4) = b->bsize;
	W32(sbp, 8) = b->fblocks;
	W32(sbp, 12) = b->ffree;
	W32(sbp, 16) = b->ffree;
	W32(sbp, 20) = b->files;
	W32(sbp, 24) = b->nfree;
	return 0;
}

/* the first function: startnext calls it with the table of kernel routines in the context */
long
hf_init()
{
	struct hctx *c = C;

	snd_init();
	c->vfsops[0] = (unsigned long)hf_inval;		/* mount */
	c->vfsops[1] = (unsigned long)hf_zero;		/* unmount */
	c->vfsops[2] = (unsigned long)hf_root;
	c->vfsops[3] = (unsigned long)hf_statfs;
	c->vfsops[4] = (unsigned long)hf_zero;		/* sync */
	c->vfsops[5] = (unsigned long)hf_inval;		/* vget */
	c->vfsops[6] = (unsigned long)hf_mountroot;
	c->vnodeops[0] = (unsigned long)hf_zero;		/* open */
	c->vnodeops[1] = (unsigned long)hf_zero;		/* close */
	c->vnodeops[2] = (unsigned long)hf_rdwr;
	c->vnodeops[3] = (unsigned long)hf_notty;	/* ioctl */
	c->vnodeops[4] = (unsigned long)hf_notty;	/* select */
	c->vnodeops[5] = (unsigned long)hf_getattr;
	c->vnodeops[6] = (unsigned long)hf_setattr;
	c->vnodeops[7] = (unsigned long)hf_access;
	c->vnodeops[8] = (unsigned long)hf_lookup;
	c->vnodeops[9] = (unsigned long)hf_create;
	c->vnodeops[10] = (unsigned long)hf_remove;
	c->vnodeops[11] = (unsigned long)hf_link;
	c->vnodeops[12] = (unsigned long)hf_rename;
	c->vnodeops[13] = (unsigned long)hf_mkdir;
	c->vnodeops[14] = (unsigned long)hf_rmdir;
	c->vnodeops[15] = (unsigned long)hf_readdir;
	c->vnodeops[16] = (unsigned long)hf_symlink;
	c->vnodeops[17] = (unsigned long)hf_readlink;
	c->vnodeops[18] = (unsigned long)hf_zero;	/* fsync */
	c->vnodeops[19] = (unsigned long)hf_inactive;
	c->vnodeops[20] = (unsigned long)hf_inval;	/* bmap */
	c->vnodeops[21] = (unsigned long)hf_inval;	/* strategy */
	c->vnodeops[22] = (unsigned long)hf_inval;
	c->vnodeops[23] = (unsigned long)hf_zero;
	c->vnodeops[24] = (unsigned long)hf_zero;	/* lockctl */
	c->vnodeops[25] = (unsigned long)hf_inval;	/* fid */
	c->vnodeops[26] = (unsigned long)hf_inval;
	c->vnodeops[27] = (unsigned long)hf_cmp;
	c->vnodeops[28] = (unsigned long)hf_inval;	/* realvp */
	c->vnodeops[29] = (unsigned long)hf_pagein;
	c->vnodeops[30] = (unsigned long)hf_pageout;
	c->vnodeops[31] = (unsigned long)hf_inval;
	c->vnodeops[32] = (unsigned long)hf_zero;
	((struct hf_keys *)HF_KEYS)->vidon = (unsigned long)hf_vidon;
	((struct hf_keys *)HF_KEYS)->vidoff = (unsigned long)hf_vidoff;
	return 0;
}
