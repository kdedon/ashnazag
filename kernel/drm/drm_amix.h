/*
 * AMIX glue for a DRM template instance: the SVR4 driver entry points,
 * the ioctl argument copy, memory, sleeps, and the maps a client finds
 * at first open (registers, frame buffer, shared area).  The display
 * backend owns the card: it hands it over with DRM(amix_attach) and
 * calls DRM(amix_front) when the front session changes.
 */
#ifndef SAREA_MAX
#define SAREA_MAX	0x2000
#endif

struct drm_amixcard DRM(card);
int DRM(pgshift) = 11;

/* only the 68040 and 68060 HAT, whose page frames are 4 KB, has it */
__asm__(".weak hat_cm_ram");
extern long hat_cm_ram;
static long *volatile DRM(hat4k) = &hat_cm_ram;

struct DRM(sleep) {
	caddr_t	chan;
	int	fired;
};

static void
DRM(waketimo)(struct DRM(sleep) *w)
{
	w->fired = 1;
	wakeup(w->chan);
}

/* 0, EINTR on a signal, EBUSY when timo ticks (0: none) pass first */
int
DRM(tsleep)(caddr_t chan, int timo)
{
	struct DRM(sleep) w;
	int id = 0, r;

	w.chan = chan;
	w.fired = 0;
	if (timo > 0)
		id = timeout(DRM(waketimo), (caddr_t)&w, timo);
	r = sleep(chan, (PZERO + 1) | PCATCH);
	if (timo > 0 && !w.fired)
		untimeout(id);
	if (r)
		return EINTR;
	return w.fired ? EBUSY : 0;
}

/* blocks freed without their size: the size sits before them */
void *
DRM(amix_malloc)(unsigned long size)
{
	unsigned long *p = (unsigned long *)kmem_alloc(size + 8, KM_NOSLEEP);

	if (p == 0)
		return 0;
	p[0] = size + 8;
	return p + 2;
}

void
DRM(amix_mfree)(void *pt)
{
	unsigned long *p = (unsigned long *)pt - 2;

	kmem_free((caddr_t)p, p[0]);
}

extern caddr_t AllocMem();
extern void FreeMem();
#define MEMF_CHIP	0x2

/*
 * Zeroed whole pages for the shared area clients map, the raw block's
 * address before them.  Chip RAM: it has no page structures, and the
 * HAT objects to a device mapping of RAM that has.
 */
void *
DRM(alloc_pages)(unsigned long size)
{
	caddr_t raw = AllocMem(size + PAGE_SIZE, MEMF_CHIP);
	unsigned long a;

	if (raw == 0)
		return 0;
	a = ((unsigned long)raw + PAGE_SIZE) & ~(unsigned long)PAGE_MASK;
	((caddr_t *)a)[-1] = raw;
	bzero(a, size);
	DRM_SAREA_SYNC();
	return (void *)a;
}

void
DRM(free_pages)(void *pt, unsigned long size)
{
	FreeMem(((caddr_t *)pt)[-1], size + PAGE_SIZE);
}

int
DRM(fuword)(void *val, caddr_t uaddr)
{
	return copyin(uaddr, (caddr_t)val, sizeof(int)) ? EFAULT : 0;
}

/* %s, %d and %x only */
int
DRM(sprintf)(char *buf, const char *fmt, ...)
{
	char **ap = (char **)(&fmt + 1), *s, *b = buf, t[12];
	unsigned long v;
	int n, base;

	for (; *fmt; fmt++) {
		if (*fmt != '%' || !fmt[1]) {
			*b++ = *fmt;
			continue;
		}
		switch (*++fmt) {
		case 's':
			for (s = *ap++; *s; )
				*b++ = *s++;
			break;
		case 'd':
		case 'x':
			v = (unsigned long)*ap++;
			base = *fmt == 'd' ? 10 : 16;
			if (base == 10 && (long)v < 0) {
				*b++ = '-';
				v = -v;
			}
			n = 0;
			do {
				t[n++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (n)
				*b++ = t[--n];
			break;
		default:
			*b++ = *fmt;
		}
	}
	*b = 0;
	return b - buf;
}

int
DRM(sysctl_init)(drm_device_t *dev)
{
	return 0;
}

int
DRM(sysctl_cleanup)(drm_device_t *dev)
{
	return 0;
}

/* the card's own mappings: nothing else is mapped */
void *
DRM(amix_mapdev)(unsigned long pa, unsigned long size)
{
	struct drm_amixcard *c = &DRM(card);

	if (pa >= c->c_physreg && pa - c->c_physreg + size <= c->c_regsize)
		return (void *)(c->c_reg + (pa - c->c_physreg));
	if (pa >= c->c_physfb && pa - c->c_physfb + size <= c->c_fbsize)
		return (void *)(c->c_fb + (pa - c->c_physfb));
	return 0;
}

extern struct dsfbh ds_fbh[];
extern int ds_owns();
extern int (*ds_leavefn)();

/* the display session behind an open /dev/fbN */
int
DRM(dsbind)(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_dsbind_t b;
	struct file *fp;
	struct vnode *vp;
	struct dssess *s;
	unsigned m;
	DRM_PRIV;

	DRM_COPY_FROM_USER_IOCTL(b, (drm_dsbind_t *)data, sizeof(b));
	if (getf(b.fd, &fp) != 0)
		return EBADF;
	vp = fp->f_vnode;
	if (vp == 0 || vp->v_type != VCHR || getmajor(vp->v_rdev) != DS_FBMAJ)
		return EINVAL;
	m = getminor(vp->v_rdev);
	if (m < 1 || m > DS_NFBH || !ds_fbh[m - 1].h_used ||
	    (s = ds_fbh[m - 1].h_sess) == 0 || s->s_dead || s == &ds_sess[0])
		return EINVAL;
	if (!ds_owns(s))
		return EACCES;
	priv->sess = s;
	priv->owner = 1;
	priv->authenticated = 1;
	b.session = s->s_id;
	b.offset = DS_VOFF(s);
	b.vram = DRM(card).c_free;
	b.vramsize = DRM(card).c_vram - DRM(card).c_free;
	DRM_COPY_TO_USER_IOCTL((drm_dsbind_t *)data, b, sizeof(b));
	return 0;
}

/* lock waiters recheck whether their session is in front */
void
DRM(amix_front)(void)
{
	wakeup((caddr_t)&DRM(softc)[0].lock.lock_queue);
}

#ifndef DRIVER_IDLE
#define DRIVER_IDLE(dev)
#endif

/*
 * Around a session switch (ds_leavefn): before it, the kernel takes the
 * lock and lets the card go idle, so nothing draws while the frame
 * moves; a holder gets a second to let go, then the switch goes ahead
 * anyway.  After it, the lock is freed and its waiters recheck the front.
 */
int
DRM(amix_leave)(struct dssess *from, int done)
{
	drm_device_t *dev = &DRM(softc)[0];
	drm_hw_lock_t *l = dev->lock.hw_lock;

	if (l == 0)
		return 0;
	if (done) {
		if (dev->lock.filp == 0 && dev->lock.pid == 0 &&
		    _DRM_LOCKING_CONTEXT(l->lock) == DRM_KERNEL_CONTEXT &&
		    _DRM_LOCK_IS_HELD(l->lock))
			DRM(lock_free)(dev, &l->lock, DRM_KERNEL_CONTEXT);
		return 0;
	}
	if (!DRM(lock_take)(&l->lock, DRM_KERNEL_CONTEXT)) {
		/* a wait given up more than a second ago starts afresh */
		if (dev->lock.leave == 0 || lbolt - dev->lock.leave > 2 * HZ)
			dev->lock.leave = lbolt ? lbolt : 1;
		if (lbolt - dev->lock.leave < HZ)
			return EBUSY;
		dev->lock.leave = 0;
		DRIVER_IDLE(dev);
		return 0;
	}
	dev->lock.leave = 0;
	dev->lock.pid = 0;
	dev->lock.filp = 0;
	DRIVER_IDLE(dev);
	return 0;
}

/* what the X server and its driver would set: bus id, registers, frame buffer, shared area */
static int
DRM(amix_mkmaps)(dev_t kdev)
{
	struct drm_amixcard *c = &DRM(card);
	drm_device_t *dev = &DRM(softc)[0];
	drm_map_t m;
	char bus[32];
	int i, e;

	DRM(sprintf)(bus, "PCI:%d:%d:%d", (int)opci_attr(c->c_dev, OPCI_BUSNO),
	    (int)opci_attr(c->c_dev, OPCI_SLOT), (int)opci_attr(c->c_dev, OPCI_FUNCTION));
	if ((dev->unique = DRM(alloc)(strlen(bus) + 1, DRM_MEM_DRIVER)) == NULL)
		return ENOMEM;
	strcpy(dev->unique, bus);
	dev->unique_len = strlen(bus);

	for (i = 0; i < 3; i++) {
		bzero((caddr_t)&m, sizeof m);
		switch (i) {
		case 0:
			m.offset = c->c_physreg;
			m.size = c->c_regsize;
			m.type = _DRM_REGISTERS;
			break;
		case 1:
			m.offset = c->c_physfb;
			m.size = c->c_fbsize;
			m.type = _DRM_FRAME_BUFFER;
			break;
		default:
			m.size = SAREA_MAX;
			m.type = _DRM_SHM;
			m.flags = _DRM_CONTAINS_LOCK;
		}
		if ((e = DRM(addmap)(kdev, DRM_IOCTL_ADD_MAP, (caddr_t)&m,
		    FREAD | FWRITE, DRM_CURPROC)) != 0)
			return e;
	}
#ifdef DRIVER_AMIX_MAPS
	DRIVER_AMIX_MAPS(dev);
#endif
	return 0;
}

/* SVR4 entry points */

/* minor 0 opens a fresh minor: its file, closed once with it */
int
DRM(svr4open)(dev_t *devp, int flag, int otyp, struct cred *cr)
{
	drm_device_t *dev = &DRM(softc)[0];
	int e, m;

	if (getminor(*devp) != 0 || DRM(card).c_reg == 0)
		return ENXIO;
	for (m = 1; m < DRM_NMINOR; m++)
		if (DRM(find_file_by_minor)(dev, m) == NULL)
			break;
	if (m == DRM_NMINOR)
		return EAGAIN;
	*devp = makedevice(getmajor(*devp), m);
	if ((e = DRM(open)(*devp, flag, otyp, DRM_CURPROC)) != 0)
		return e;
	if (TAILQ_FIRST(dev->maplist) == NULL && (e = DRM(amix_mkmaps)(*devp)) != 0) {
		DRM(close)(*devp, flag, otyp, DRM_CURPROC);
		return e;
	}
	return 0;
}

int
DRM(svr4close)(dev_t dev, int flag, int otyp, struct cred *cr)
{
	return DRM(close)(dev, flag, otyp, DRM_CURPROC);
}

int
DRM(svr4read)(dev_t dev, struct uio *uio, struct cred *cr)
{
	return 0;
}

int
DRM(svr4write)(dev_t dev, struct uio *uio, struct cred *cr)
{
	return 0;
}

/* the BSD kernel copies arguments by the size and direction in cmd; here the driver does */
int
DRM(svr4ioctl)(dev_t dev, int cmd, int arg, int mode, struct cred *cr, int *rvalp)
{
	long buf[(IOCPARM_MASK + 4) / 4];
	int size = IOCPARM_LEN(cmd), e;

	if ((cmd >> 8 & 0xff) != DRM_IOCTL_BASE)
		return EINVAL;
	if (cmd & IOC_IN) {
		if (copyin((caddr_t)arg, (caddr_t)buf, size))
			return EFAULT;
	} else
		bzero((caddr_t)buf, size);
	e = DRM(ioctl)(dev, (u_long)cmd, (caddr_t)buf, mode, DRM_CURPROC);
	if (e == 0 && (cmd & IOC_OUT) && copyout((caddr_t)buf, (caddr_t)arg, size))
		e = EFAULT;
	return e;
}

int
DRM(svr4mmap)(dev_t dev, off_t off, int prot)
{
	return DRM(mmap)(dev, off, prot);
}

/* the card from its display backend */
int
DRM(amix_attach)(struct drm_amixcard *c)
{
	drm_device_t *dev = &DRM(softc)[0];
	int e;

	if (DRM(hat4k))
		DRM(pgshift) = 12;
	DRM(card) = *c;
	bzero((caddr_t)dev, sizeof *dev);
	ds_leavefn = DRM(amix_leave);
	if ((e = DRM(init)(dev)) != 0) {
		DRM(card).c_reg = 0;
		return e;
	}
	return 0;
}

int
DRM(amix_detach)(void)
{
	drm_device_t *dev = &DRM(softc)[0];

	if (dev->open_count)
		return EBUSY;
	ds_leavefn = 0;
	DRM(cleanup)(dev);
	DRM(card).c_reg = 0;
	return 0;
}
