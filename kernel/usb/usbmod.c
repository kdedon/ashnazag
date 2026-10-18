/*
 * usbmod.c - the USB stack as a loadable module: /dev/usb (major 64),
 * the character device user programs drive the stack through (usbtool:
 * poll the hubs, list devices and storage, read and write blocks), and
 * the registry the host controller modules (rapidroad) register with.
 *
 * Everything runs in process context: the module loads on the first
 * open of /dev/usb or as a dependency of a controller module, the
 * controller module registers its root hub when it is loaded (modadmin
 * -l rapidroad) and enumerates what is plugged in, and a user process
 * polls the hubs (usbtool daemon) for later plugging and unplugging.
 * The stack's waits are sleeps.  ubioc.h is the ioctl interface.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/systm.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/cred.h"
#include "sys/file.h"
#include "sys/open.h"
#include "sys/sysmacros.h"
#include "sys/kmem.h"
#include "sys/moddefs.h"

#include "ub.h"
#include "usb.h"
#include "hub.h"
#include "usb_api.h"
#include "ubstor.h"
#include "ubioc.h"

extern int drv_priv(), nodev();
extern clock_t drv_usectohz();
extern int copyin(), copyout();
extern struct ucdif *allucdifs;

static int usb_open_count;
static int usb_flag;

/* ---- the kernel side of ub.h ---------------------------------------- */

void
ub_memset(p, c, n)
char *p;
int c;
unsigned n;
{
	while (n-- > 0)
		*p++ = c;
}

char *
strcat(d, s)
char *d, *s;
{
	char *r = d;

	while (*d)
		d++;
	while ((*d++ = *s++) != 0)
		;
	return r;
}

/* the little of sprintf the stack's dumps use: %s %c %d %ld %u %lu %x %lx %p */
int
sprintf(buf, fmt, a1, a2, a3, a4, a5, a6, a7, a8)
char *buf, *fmt;
long a1, a2, a3, a4, a5, a6, a7, a8;
{
	long args[8], v;
	char *o = buf, *s, tmp[24];
	int ai = 0, lng, i, neg;
	unsigned long u;

	args[0] = a1; args[1] = a2; args[2] = a3; args[3] = a4;
	args[4] = a5; args[5] = a6; args[6] = a7; args[7] = a8;
	while (*fmt) {
		if (*fmt != '%') {
			*o++ = *fmt++;
			continue;
		}
		fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			fmt++;
		lng = 0;
		if (*fmt == 'l') {
			lng = 1;
			fmt++;
		}
		v = ai < 8 ? args[ai++] : 0;
		switch (*fmt++) {
		case 's':
			for (s = (char *)v; s && *s; s++)
				*o++ = *s;
			break;
		case 'c':
			*o++ = (char)v;
			break;
		case 'd':
			if (!lng)
				v = (int)v;
			neg = v < 0;
			u = neg ? -v : v;
			goto num10;
		case 'u':
			u = lng ? (unsigned long)v : (unsigned)v;
			neg = 0;
		num10:
			i = 0;
			do {
				tmp[i++] = '0' + u % 10;
			} while ((u /= 10) != 0);
			if (neg)
				*o++ = '-';
			while (i > 0)
				*o++ = tmp[--i];
			break;
		case 'x':
		case 'p':
			u = lng || fmt[-1] == 'p' ? (unsigned long)v : (unsigned)v;
			i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[u & 15];
			} while ((u >>= 4) != 0);
			while (i > 0)
				*o++ = tmp[--i];
			break;
		case '%':
			*o++ = '%';
			ai--;
			break;
		default:
			*o++ = '?';
		}
	}
	*o = 0;
	return o - buf;
}

void
ub_udelay(us)
long us;
{
	drv_usecwait(us);
}

/* at least one tick: the stack's delays are USB settling times */
void
ub_mdelay(ms)
long ms;
{
	long t = drv_usectohz(ms * 1000);

	delay(t > 0 ? t : 1);
}

/* ---- the device ------------------------------------------------------ */

/* ARGSUSED */
int
usbopen(devp, flag, otyp, crp)
dev_t *devp;
int flag, otyp;
struct cred *crp;
{
	if (getminor(*devp) != 0 || otyp != OTYP_CHR)
		return ENXIO;
	usb_open_count++;
	return 0;
}

/* ARGSUSED */
int
usbclose(dev, flag, otyp, crp)
dev_t dev;
int flag, otyp;
struct cred *crp;
{
	if (usb_open_count > 0)
		usb_open_count--;
	return 0;
}

static void
usb_poll_hubs()
{
	int i;

	for (i = 0; i < USB_MAX_HUB; i++) {
		struct usb_hub_device *hub = usb_get_hub_index(i);

		if (hub)
			usb_hub_events(hub);
	}
}

static void
usb_strcpy_n(d, s, n)
char *d, *s;
int n;
{
	while (--n > 0 && *s)
		*d++ = *s++;
	*d = 0;
}

static int
usb_devs(arg)
caddr_t arg;
{
	static struct ub_devinfo info[UB_NDEV];
	int i;

	ub_memset((char *)info, 0, sizeof info);
	for (i = 0; i < USB_MAX_DEVICE && i < UB_NDEV; i++) {
		struct usb_device *d = usb_get_dev_index(i);

		if (d == (struct usb_device *)0)
			continue;
		info[i].present = 1;
		info[i].addr = d->devnum;
		info[i].hub = d->maxchild != 0;
		info[i].vid = d->descriptor.idVendor;
		info[i].pid = d->descriptor.idProduct;
		info[i].class = d->descriptor.bDeviceClass;
		usb_strcpy_n(info[i].mf, d->mf, sizeof info[i].mf);
		usb_strcpy_n(info[i].prod, d->prod, sizeof info[i].prod);
		usb_strcpy_n(info[i].serial, d->serial, sizeof info[i].serial);
	}
	return copyout((caddr_t)info, arg, sizeof info) ? EFAULT : 0;
}

static int
usb_storlist(arg)
caddr_t arg;
{
	static struct ub_storinfo info[UB_NSTOR];
	int i;

	ub_memset((char *)info, 0, sizeof info);
	for (i = 0; i < UB_STOR_MAX && i < UB_NSTOR; i++) {
		struct ub_stor *s = &ub_stor[i];

		if (s->dev == (struct usb_device *)0)
			continue;
		info[i].present = 1;
		info[i].ready = s->ready;
		info[i].removable = s->removable;
		info[i].devaddr = s->dev->devnum;
		info[i].blocks = s->blocks;
		info[i].blocksize = s->blocksize;
		usb_strcpy_n(info[i].vendor, s->vendor, sizeof info[i].vendor);
		usb_strcpy_n(info[i].product, s->product, sizeof info[i].product);
		usb_strcpy_n(info[i].rev, s->rev, sizeof info[i].rev);
	}
	return copyout((caddr_t)info, arg, sizeof info) ? EFAULT : 0;
}

static int
usb_rw(arg, write)
caddr_t arg;
int write;
{
	struct ub_rw rw;
	struct ub_stor *s;
	char *buf;
	unsigned long bytes;
	int r = 0;

	if (copyin(arg, (caddr_t)&rw, sizeof rw))
		return EFAULT;
	if (rw.unit < 0 || rw.unit >= UB_STOR_MAX)
		return EINVAL;
	s = &ub_stor[rw.unit];
	if (s->dev == (struct usb_device *)0 || !s->ready)
		return ENXIO;
	bytes = rw.n * s->blocksize;
	if (rw.n == 0 || bytes > UB_RWMAX || rw.lba + rw.n > s->blocks)
		return EINVAL;
	buf = kmem_alloc(bytes, KM_SLEEP);
	if (buf == (char *)0)
		return ENOMEM;
	if (write && copyin((caddr_t)rw.buf, buf, bytes))
		r = EFAULT;
	else if ((write ? ub_stor_write(s, rw.lba, rw.n, (unsigned char *)buf)
	    : ub_stor_read(s, rw.lba, rw.n, (unsigned char *)buf)) < 0)
		r = EIO;
	else if (!write && copyout(buf, (caddr_t)rw.buf, bytes))
		r = EFAULT;
	kmem_free(buf, bytes);
	return r;
}

/* ARGSUSED */
int
usbioctl(dev, cmd, arg, mode, crp, rvalp)
dev_t dev;
int cmd, arg, mode;
struct cred *crp;
int *rvalp;
{
	switch (cmd) {
	case UBIOC_STATUS:
		*rvalp = allucdifs ? UB_RUNNING : UB_NOCHIP;
		return 0;
	case UBIOC_START:	/* the stack runs from the load; a poll */
	case UBIOC_POLL:
		if (allucdifs == (struct ucdif *)0)
			return ENXIO;
		usb_poll_hubs();
		return 0;
	case UBIOC_DEVS:
		return usb_devs((caddr_t)arg);
	case UBIOC_STOR:
		return usb_storlist((caddr_t)arg);
	case UBIOC_RD:
		return usb_rw((caddr_t)arg, 0);
	case UBIOC_WR:
		if (drv_priv(crp))
			return EPERM;
		return usb_rw((caddr_t)arg, 1);
	}
	return EINVAL;
}

/* ---- the module ------------------------------------------------------ */

struct mod_drv_data usb_drvdata[] = {
	{ { 0 }, 0, 0,
	  { usbopen, usbclose, nodev, nodev, usbioctl, nodev, nodev, nodev,
	    0, 0, 0, 0, &usb_flag }, 64, 1 }
};

static int
usb_load()
{
	usb_init();
	ub_stor_init();
	return 0;
}

/* a registered controller holds the module, as does an open */
static int
usb_unload()
{
	if (usb_open_count || allucdifs)
		return EBUSY;
	return 0;
}

MOD_DRV_WRAPPER(usb, usb_load, usb_unload, 0, "USB stack and mass storage");
