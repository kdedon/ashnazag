/*
 * dsdev.c -- /dev/fbN, /dev/kbd and /dev/mouse.
 *
 * Every open of minor 0 clones: the file gets its own minor (handle
 * index + 1), so close, mmap and the session belong to that file.  Its
 * last close (after its mappings are gone) ends the session.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/kmem.h"
#include "sys/cred.h"
#include "sys/uio.h"
#include "sys/file.h"
#include "sys/vnode.h"
#include "sys/proc.h"
#include "sys/disp.h"
#include "sys/session.h"
#include "sys/conf.h"
#include "sys/stream.h"
#include "fbcons.h"
#include "adb.h"
#include "ds.h"

extern struct fbpmode fbp_mode[];
extern int fbp_nmode, fbp_cur;
extern int getf();
extern struct streamtab coinfo;

struct dsfbh ds_fbh[DS_NFBH];
struct dsevh ds_evh[DS_NEVH];
static struct pollhead ds_noph;

#define PRI	((PZERO + 1) | PCATCH)

static struct dsfbh *
ds_fbhandle(dev)
dev_t dev;
{
	register unsigned m = getminor(dev);

	if (m < 1 || m > DS_NFBH || !ds_fbh[m - 1].h_used)
		return 0;
	return &ds_fbh[m - 1];
}

struct dssess *
ds_fbsess(dev)
dev_t dev;
{
	register struct dsfbh *h = ds_fbhandle(dev);

	return h ? h->h_sess : 0;
}

static struct dsevh *
ds_evhandle(dev)
dev_t dev;
{
	register unsigned m = getminor(dev);

	if (m < 1 || m > DS_NEVH || !ds_evh[m - 1].e_used)
		return 0;
	return &ds_evh[m - 1];
}

/* ------------------------------------------------------------ /dev/fb */

/* by the calling process: fds are inherited and passed */
int
ds_owns(s)
struct dssess *s;
{
	register struct cred *cr = curproc->p_cred;

	return s->s_uid == cr->cr_uid || drv_priv(cr) == 0;
}

int
ds_fbopen(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	register int i, err;

	if (getminor(*devp) != 0)
		return ENXIO;
	if ((err = ds_init()) != 0)
		return err;
	for (i = 0; i < DS_NFBH; i++)
		if (!ds_fbh[i].h_used) {
			ds_fbh[i].h_used = 1;
			ds_fbh[i].h_sess = 0;
			*devp = makedevice(getmajor(*devp), i + 1);
			return 0;
		}
	return EAGAIN;
}

int
ds_fbclose(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	register struct dsfbh *h = ds_fbhandle(dev);
	register struct dssess *s;

	if (h == 0)
		return 0;
	s = h->h_sess;
	h->h_sess = 0;
	h->h_used = 0;
	if (s) {
		ds_endsess(s);
		ds_sessgc(s);
	}
	return 0;
}

/* notes: FBN_HIDDEN, FBN_SHOWN, whole records */
int
ds_fbread(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	register struct dsfbh *h = ds_fbhandle(dev);
	register struct dssess *s;
	struct fbnote n;
	register int x, err = 0, got = 0;

	if (h == 0 || (s = h->h_sess) == 0)
		return EINVAL;
	if (!ds_owns(s))
		return EACCES;
	while (uio->uio_resid >= sizeof n) {
		x = DS_SPL(DS_HI);
		if (s->s_nget == s->s_nput) {
			DS_SPLX(x);
			if (got || s->s_dead)
				break;
			if (uio->uio_fmode & (FNDELAY | FNONBLOCK))
				return EAGAIN;
			if (sleep((caddr_t)s->s_note, PRI))
				return EINTR;
			if (h->h_sess != s)
				return 0;
			continue;
		}
		n = s->s_note[s->s_nget];
		s->s_nget = (s->s_nget + 1) % DS_NNOTE;
		DS_SPLX(x);
		if ((err = uiomove((caddr_t)&n, sizeof n, UIO_READ, uio)) != 0)
			break;
		got = 1;
	}
	return err;
}

int
ds_fbpoll(dev, events, anyyet, reventsp, phpp)
dev_t dev;
short events;
int anyyet;
short *reventsp;
struct pollhead **phpp;
{
	register struct dssess *s = ds_fbsess(dev);

	*reventsp = 0;
	if (s && s->s_nget != s->s_nput)
		*reventsp = events & (POLLIN | POLLRDNORM);
	if (s && s->s_dead)
		*reventsp |= POLLHUP;
	if (*reventsp == 0 && !anyyet)
		*phpp = s ? &s->s_ph : &ds_noph;
	return 0;
}

/* page frame at offset off of the caller's session, -1 if none */
int
ds_fbmmap(dev, off, prot)
dev_t dev;
off_t off;
int prot;
{
	register struct dssess *s = ds_fbsess(dev);

	if (s == 0 || s->s_dead || off < 0 || off >= s->s_size)
		return -1;
	if (s == ds_front)
		return (ds_disp.d_page + off) >> DS_PGSHIFT;
	return s->s_pfn[off >> DS_PGSHIFT];
}

/*
 * May cr bring a session to the front: privileged, the owner of the
 * front session, or the person at the machine (controlling terminal
 * the console, whose output is the screen).
 */
int
ds_mayfront(cr)
struct cred *cr;
{
	dev_t d;

	if (drv_priv(cr) == 0)
		return 1;
	if (ds_front != &ds_sess[0] && ds_front->s_uid == cr->cr_uid)
		return 1;
	d = cttydev(curproc);
	return d != NODEV && getmajor(d) < cdevcnt &&
	    cdevsw[getmajor(d)].d_str == &coinfo && getminor(d) == 0;
}

static int
ds_gmodes(arg)
caddr_t arg;
{
	struct fbmodes ms;
	struct fbmodeinfo mi;
	register struct fbinfo *fi = &ds_disp.d_info;
	register unsigned long i, n;

	if (copyin(arg, (caddr_t)&ms, sizeof ms))
		return EFAULT;
	n = (fbp_cur >= 0 && fbp_cur < fbp_nmode) ? fbp_nmode : 1;
	for (i = 0; i < n && i < ms.ms_count; i++) {
		if (n == 1) {
			mi.mi_id = fi->fi_mode;
			mi.mi_width = fi->fi_width;
			mi.mi_height = fi->fi_height;
			mi.mi_depth = fi->fi_depth;
			mi.mi_rowbytes = fi->fi_rowbytes;
			mi.mi_offset = fi->fi_offset;
			mi.mi_flags = FBM_CURRENT;
		} else {
			mi.mi_id = fbp_mode[i].pm_id;
			mi.mi_width = fbp_mode[i].pm_width;
			mi.mi_height = fbp_mode[i].pm_height;
			mi.mi_depth = fbp_mode[i].pm_depth;
			mi.mi_rowbytes = fbp_mode[i].pm_row;
			mi.mi_offset = fbp_mode[i].pm_off;
			mi.mi_flags = i == fbp_cur ? FBM_CURRENT : 0;
		}
		if (copyout((caddr_t)&mi, (caddr_t)(ms.ms_modes + i), sizeof mi))
			return EFAULT;
	}
	ms.ms_count = n;
	if (copyout((caddr_t)&ms, arg, sizeof ms))
		return EFAULT;
	return 0;
}

static int
ds_cmapio(s, arg, put)
struct dssess *s;
caddr_t arg;
int put;
{
	struct fbcmap cm;
	register unsigned short *b;
	register int n, sz, err = 0;

	if (copyin(arg, (caddr_t)&cm, sizeof cm))
		return EFAULT;
	n = cm.cm_count;
	if ((unsigned long)cm.cm_start + n > ds_disp.d_info.fi_cmapsize)
		return EINVAL;
	if (put && !(ds_disp.d_info.fi_flags & FBF_CMAP))
		return ENXIO;
	if (n == 0)
		return 0;
	sz = n * sizeof (unsigned short);
	if (!put) {
		if (copyout((caddr_t)&s->s_cmap[0][cm.cm_start], (caddr_t)cm.cm_red, sz) ||
		    copyout((caddr_t)&s->s_cmap[1][cm.cm_start], (caddr_t)cm.cm_green, sz) ||
		    copyout((caddr_t)&s->s_cmap[2][cm.cm_start], (caddr_t)cm.cm_blue, sz))
			return EFAULT;
		return 0;
	}
	b = (unsigned short *)kmem_alloc(3 * 256 * sizeof (unsigned short), KM_SLEEP);
	if (copyin((caddr_t)cm.cm_red, (caddr_t)b, sz) ||
	    copyin((caddr_t)cm.cm_green, (caddr_t)(b + 256), sz) ||
	    copyin((caddr_t)cm.cm_blue, (caddr_t)(b + 512), sz))
		err = EFAULT;
	else
		ds_setcmap(s, (int)cm.cm_start, n, b, b + 256, b + 512);
	kmem_free((caddr_t)b, 3 * 256 * sizeof (unsigned short));
	return err;
}

static int
ds_acquire(h, arg, cr)
register struct dsfbh *h;
caddr_t arg;
struct cred *cr;
{
	struct fbacq a;
	register struct dssess *s;
	int err;

	if (copyin(arg, (caddr_t)&a, sizeof a))
		return EFAULT;
	if (a.fa_kind != FBK_USER || (a.fa_flags & ~(FBA_FRONT | FBA_VIDEL)))
		return EINVAL;
	if (h->h_sess)
		return EBUSY;
	if ((a.fa_flags & (FBA_FRONT | FBA_VIDEL)) && !ds_mayfront(cr))
		return EPERM;
	a.fa_name[sizeof a.fa_name - 1] = 0;
	if ((s = ds_mksess((long)cr->cr_uid, a.fa_name, &err, a.fa_flags & FBA_VIDEL)) == 0)
		return err;
	h->h_sess = s;
	a.fa_id = s->s_id;
	if (a.fa_flags & FBA_FRONT)
		(void)ds_switch(s);
	if (copyout((caddr_t)&a, arg, sizeof a))
		return EFAULT;
	return 0;
}

int
ds_fbioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd, arg, mode;
struct cred *cr;
int *rvalp;
{
	register struct dsfbh *h = ds_fbhandle(dev);
	register struct dssess *s, *t;
	struct fbstate st;
	struct fbinfo fi;
	unsigned long v;

	if (h == 0)
		return ENXIO;
	cr = curproc->p_cred;		/* the caller may hold an inherited fd */
	s = h->h_sess;
	if (s && (s->s_dead || !ds_owns(s)))
		s = 0;
	switch (cmd) {
	case FBIOGINFO:
		fi = ds_disp.d_info;
		if (s)
			fi.fi_size = s->s_size;
		if (copyout((caddr_t)&fi, (caddr_t)arg, sizeof fi))
			return EFAULT;
		return 0;
	case FBIOGMODES:
		return ds_gmodes((caddr_t)arg);
	case FBIOSMODE:
		return ENXIO;
	case FBIOGETCMAP:
	case FBIOPUTCMAP:
		if (s == 0)
			return EINVAL;
		return ds_cmapio(s, (caddr_t)arg, cmd == FBIOPUTCMAP);
	case FBIOACQUIRE:
		return ds_acquire(h, (caddr_t)arg, cr);
	case FBIORELEASE:
		if (s == 0)
			return EINVAL;
		h->h_sess = 0;
		ds_endsess(s);
		ds_sessgc(s);
		return 0;
	case FBIOSWITCH:
		if ((t = ds_byid((long)arg)) == 0)
			return EINVAL;
		if (!ds_mayfront(cr))
			return EPERM;
		return ds_switch(t);
	case FBIOGSTATE:
		st.st_session = s ? s->s_id : -1;
		st.st_front = ds_front->s_id;
		st.st_serial = ds_serial;
		if (copyout((caddr_t)&st, (caddr_t)arg, sizeof st))
			return EFAULT;
		return 0;
	case FBIOVBLWAIT:
		if (arg < 1 || arg > FB_MAXVBLWAIT)
			return EINVAL;
		v = ds_vblcount + arg;
		while ((long)(ds_vblcount - v) < 0)
			if (sleep((caddr_t)&ds_vblcount, PRI))
				return EINTR;
		return 0;
	case FBIOGVBL:
		v = ds_vblcount;
		if (copyout((caddr_t)&v, (caddr_t)arg, sizeof v))
			return EFAULT;
		return 0;
	case FBIOBLANK:
		if (s == 0)
			return EINVAL;
		if (!(ds_disp.d_info.fi_flags & FBF_BLANK))
			return ENXIO;
		ds_setblank(s, arg != 0);
		return 0;
	case FBIOCACHE:
		if (s == 0)
			return EINVAL;
		if (arg != FBC_WT && arg != FBC_CI)
			return EINVAL;
		if (arg == FBC_WT && !ds_disp.d_dafb)
			return ENXIO;
		s->s_cache = arg;
		return 0;
#ifdef DS_ATARI
	case FBIOVIDEL:
		if (s == 0)
			return EINVAL;
		return ds_vidpass(s, (unsigned long)arg);
#endif
	}
	return EINVAL;
}

/* ---------------------------------------------------- /dev/kbd, mouse */

int
ds_evopen(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	register struct dsevh *e;
	register struct inev *q;
	register int i, err;

	if (getminor(*devp) != 0)
		return ENXIO;
	if ((err = ds_init()) != 0)
		return err;
	q = (struct inev *)kmem_zalloc(DS_QLEN * sizeof (struct inev), KM_SLEEP);
	for (i = 0; i < DS_NEVH; i++)
		if (!ds_evh[i].e_used) {
			e = &ds_evh[i];
			bzero((caddr_t)e, sizeof *e);
			e->e_q = q;
			e->e_mouse = getmajor(*devp) == DS_MSMAJ;
			e->e_used = 1;
			*devp = makedevice(getmajor(*devp), i + 1);
			return 0;
		}
	kmem_free((caddr_t)q, DS_QLEN * sizeof (struct inev));
	return EAGAIN;
}

int
ds_evclose(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	register struct dsevh *e = ds_evhandle(dev);
	register struct inev *q;
	register int x;

	if (e == 0)
		return 0;
	x = DS_SPL(DS_HI);
	e->e_sess = 0;
	q = e->e_q;
	e->e_q = 0;
	e->e_used = 0;
	DS_SPLX(x);
	kmem_free((caddr_t)q, DS_QLEN * sizeof (struct inev));
	return 0;
}

/*
 * One event to every input file of kind `mouse' bound to s.  A full
 * queue gets one IE_DROP and nothing more until the reader empties it.
 * The console's keys go only to its tty.
 */
void
ds_evpost(s, mouse, type, code, value, sec, usec)
struct dssess *s;
int mouse, type, code;
long value, sec, usec;
{
	register struct dsevh *e;
	register struct inev *v;
	register int i, x, n;

	if (s == &ds_sess[0])
		return;
	x = DS_SPL(DS_HI);
	if (s->s_kin)
		(*s->s_kin)(s, type, code, value);
	for (i = 0; i < DS_NEVH; i++) {
		e = &ds_evh[i];
		if (!e->e_used || e->e_sess != s || e->e_mouse != mouse || e->e_drop)
			continue;
		n = (e->e_put - e->e_get + DS_QLEN) % DS_QLEN;
		v = &e->e_q[e->e_put];
		if (n >= DS_QLEN - 2) {
			e->e_drop = 1;
			type = IE_DROP;
		}
		v->ie_type = type;
		v->ie_unit = 0;
		v->ie_code = type == IE_DROP ? 0 : code;
		v->ie_value = type == IE_DROP ? 0 : value;
		v->ie_sec = sec;
		v->ie_usec = usec;
		e->e_put = (e->e_put + 1) % DS_QLEN;
		wakeup((caddr_t)e);
		pollwakeup(&e->e_ph, POLLIN | POLLRDNORM);
	}
	DS_SPLX(x);
}

int
ds_evread(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	register struct dsevh *e = ds_evhandle(dev);
	struct inev v;
	register int x, err = 0, got = 0;

	if (e == 0)
		return ENXIO;
	if (uio->uio_resid < sizeof v || e->e_sess == 0)
		return EINVAL;
	if (!ds_owns(e->e_sess))
		return EACCES;
	while (uio->uio_resid >= sizeof v) {
		x = DS_SPL(DS_HI);
		if (e->e_get == e->e_put) {
			e->e_drop = 0;
			DS_SPLX(x);
			if (got || e->e_sess == 0)
				break;
			if (uio->uio_fmode & (FNDELAY | FNONBLOCK))
				return EAGAIN;
			if (sleep((caddr_t)e, PRI))
				return EINTR;
			if (!e->e_used)
				return 0;
			continue;
		}
		v = e->e_q[e->e_get];
		e->e_get = (e->e_get + 1) % DS_QLEN;
		DS_SPLX(x);
		if ((err = uiomove((caddr_t)&v, sizeof v, UIO_READ, uio)) != 0)
			break;
		got = 1;
	}
	return err;
}

int
ds_evpoll(dev, events, anyyet, reventsp, phpp)
dev_t dev;
short events;
int anyyet;
short *reventsp;
struct pollhead **phpp;
{
	register struct dsevh *e = ds_evhandle(dev);

	*reventsp = 0;
	if (e && e->e_get != e->e_put)
		*reventsp = events & (POLLIN | POLLRDNORM);
	else if (e && e->e_sess == 0)
		*reventsp = POLLHUP;
	if (*reventsp == 0 && !anyyet)
		*phpp = e ? &e->e_ph : &ds_noph;
	return 0;
}

int
ds_evioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd, arg, mode;
struct cred *cr;
int *rvalp;
{
	register struct dsevh *e = ds_evhandle(dev);
	register struct dsfbh *h;
	register struct vnode *vp;
	struct evinfo ei;
	unsigned char keys[16];
	struct file *fp;
	register int i, x;

	if (e == 0)
		return ENXIO;
	switch (cmd) {
	case EVIOCGINFO:
		bzero((caddr_t)&ei, sizeof ei);
		ei.ei_kset = EVK_ADB;
#ifdef DS_ATARI
		if (e->e_sess && e->e_sess->s_vid)
			ei.ei_kset = EVK_IKBD;	/* IKBD scancodes */
#endif
		ei.ei_flags = EVF_CAPSLATCH;
		for (i = 0; i < 16; i++)
			if (adb_dev[i].d_orig == (e->e_mouse ? ADB_ADDR_MOUSE : ADB_ADDR_KBD)) {
				ei.ei_id = adb_dev[i].d_handler;
				break;
			}
		if (copyout((caddr_t)&ei, (caddr_t)arg, sizeof ei))
			return EFAULT;
		return 0;
	case EVIOCBIND:
		if (getf(arg, &fp) != 0)
			return EBADF;
		vp = fp->f_vnode;
		if (vp == 0 || vp->v_type != VCHR || getmajor(vp->v_rdev) != DS_FBMAJ ||
		    (h = ds_fbhandle(vp->v_rdev)) == 0 || h->h_sess == 0 || h->h_sess->s_dead)
			return EINVAL;
		if (!ds_owns(h->h_sess))
			return EACCES;
		x = DS_SPL(DS_HI);
		e->e_sess = h->h_sess;
		e->e_get = e->e_put = e->e_drop = 0;
		DS_SPLX(x);
		return 0;
	case EVIOCGKEYS:
		bzero((caddr_t)keys, sizeof keys);
		x = DS_SPL(DS_HI);
		if (e->e_sess)
			for (i = 0; i < 16; i++)
				keys[i] = e->e_sess->s_keys[i];
		DS_SPLX(x);
		if (copyout((caddr_t)keys, (caddr_t)arg, sizeof keys))
			return EFAULT;
		return 0;
	case EVIOCSLED:
		if (e->e_mouse || (arg & ~7))
			return EINVAL;
		if (e->e_sess != ds_front)
			return EPERM;
		adbkbd_setleds(arg);
		return 0;
	}
	return EINVAL;
}
