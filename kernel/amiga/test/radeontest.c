/*
 * radeontest -- the radeon DRM on the card, step by step, in an RGB565
 * display session: version, session bind, maps, the kernel's layout,
 * parameters, the lock, the command processor going idle, a green
 * clear through the ring, a texture upload through a DMA buffer read
 * back byte for byte, the interrupt, and no lock while hidden.  Each
 * step prints "ok" or what it saw; the last line is "radeontest: ok".
 *
 *   radeontest [seconds]     hold the picture that long (default 30)
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "dsio.h"
#include "drm.h"

/* the kernel's lock word and its cache line, as the shared area starts */
typedef struct {
	volatile unsigned int lock;
	char padding[60];
} drm_hw_lock_t;

#include "drm_sarea.h"
#include "radeon_drm.h"
#include "radeon_amix.h"

#define	MODE16		0xb0		/* the RTG mode at 16 bits a pixel */
#define	TXFORMAT_ARGB8888 6
#define	TW		64		/* texture upload: texels a row */
#define	TH		4

static int fails;

static void
alarmed(s)
int s;
{
}

static void
die(what)
char *what;
{
	fprintf(stderr, "radeontest: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void
check(what, good)
char *what;
int good;
{
	printf("radeontest: %s %s\n", what, good ? "ok" : "FAILED");
	fflush(stdout);
	if (!good)
		fails++;
}

static void *
map(fd, off, size)
int fd;
unsigned long off, size;
{
	void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);

	if (p == (void *)-1)
		die("mmap");
	return p;
}

static int
param(fd, p)
int fd, p;
{
	drm_radeon_getparam_t g;
	int v = -1;

	g.param = p;
	g.value = &v;
	if (ioctl(fd, DRM_IOCTL_RADEON_GETPARAM, &g) < 0)
		return -1;
	return v;
}

int
main(argc, argv)
int argc;
char **argv;
{
	static unsigned char tex[TW * 4 * TH];
	struct fbacq a;
	struct fbinfo fi;
	drm_version_t v;
	drm_dsbind_t b;
	drm_map_t m, fbm, shm;
	drm_ctx_t ctx;
	drm_lock_t lk;
	drm_radeon_clear_t cl;
	drm_radeon_clear_rect_t rect;
	drm_radeon_texture_t tx;
	drm_radeon_tex_image_t im;
	drm_radeon_irq_emit_t ie;
	drm_radeon_irq_wait_t iw;
	drm_radeon_sarea_t *rs;
	radeon_amix_layout_t *lp;
	char name[32], date[32], desc[64];
	volatile unsigned short *fb;
	volatile unsigned char *vt;
	volatile unsigned int *lock;
	char *sarea;
	unsigned long w, h, x, y;
	int fd, dfd, i, seq, secs = argc > 1 ? atoi(argv[1]) : 30;

	if ((fd = open("/dev/fb0", O_RDWR)) < 0)
		die("/dev/fb0");
	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = FBA_FRONT;
	strcpy(a.fa_name, "radeontest");
	if (ioctl(fd, FBIOACQUIRE, &a) < 0)
		die("FBIOACQUIRE");
	if (ioctl(fd, FBIOSMODE, MODE16) < 0)
		die("FBIOSMODE");
	if (ioctl(fd, FBIOGINFO, &fi) < 0)
		die("FBIOGINFO");
	w = fi.fi_width;
	h = fi.fi_height;

	if ((dfd = open("/dev/dri/card0", O_RDWR)) < 0)
		die("/dev/dri/card0");
	memset(&v, 0, sizeof v);
	v.name = name; v.name_len = sizeof name - 1;
	v.date = date; v.date_len = sizeof date - 1;
	v.desc = desc; v.desc_len = sizeof desc - 1;
	if (ioctl(dfd, DRM_IOCTL_VERSION, &v) < 0)
		die("VERSION");
	name[v.name_len < sizeof name ? v.name_len : sizeof name - 1] = 0;
	printf("radeontest: version %d.%d.%d %s\n", v.version_major, v.version_minor,
	    v.version_patchlevel, name);
	check("driver radeon 1.7", strcmp(name, "radeon") == 0 && v.version_major == 1 &&
	    v.version_minor >= 7);
	b.fd = fd;
	if (ioctl(dfd, DRM_IOCTL_DSBIND, &b) < 0)
		die("DSBIND");
	printf("radeontest: session %ld frame 0x%lx, free VRAM 0x%lx+0x%lx\n",
	    b.session, b.offset, b.vram, b.vramsize);

	memset(&fbm, 0, sizeof fbm); shm = fbm;
	for (i = 0; ; i++) {
		memset(&m, 0, sizeof m);
		m.offset = i;
		if (ioctl(dfd, DRM_IOCTL_GET_MAP, &m) < 0)
			break;
		printf("radeontest: map %d offset 0x%lx size 0x%lx type %d flags 0x%x\n", i,
		    m.offset, m.size, (int)m.type, (int)m.flags);
		if (m.type == _DRM_FRAME_BUFFER)
			fbm = m;
		else if (m.type == _DRM_SHM)
			shm = m;
	}
	check("maps: registers, frame buffer, shared area, ring, status, buffers",
	    i == 6 && fbm.size && shm.size);
	if (!fbm.size || !shm.size)
		return 1;
	fb = map(dfd, fbm.offset + b.offset, (w * h * 2 + 4095) & ~4095UL);
	sarea = map(dfd, shm.offset, shm.size);
	lock = (volatile unsigned int *)sarea;
	rs = (drm_radeon_sarea_t *)(sarea + sizeof(drm_sarea_t));
	lp = (radeon_amix_layout_t *)(sarea + RADEON_AMIX_LAYOUT);
	printf("radeontest: device %x family %d back 0x%x depth 0x%x+0x%x textures 0x%x+0x%x gran %d irq %d\n",
	    lp->deviceID, lp->chipFamily, lp->backOffset, lp->depthOffset, lp->depthSize,
	    lp->textureOffset, lp->textureSize, lp->log2TexGran, lp->irq);
	check("CP started by the kernel", lp->backSize != 0);
	if (!lp->backSize)
		return 1;
	printf("radeontest: CP %s, buffer offset 0x%x, base 0x%x, irq %d, last frame %d\n",
	    lp->cpInVram ? "in VRAM" : "on PCI GART",
	    param(dfd, RADEON_PARAM_AGP_BUFFER_OFFSET), param(dfd, RADEON_PARAM_AGP_BASE),
	    param(dfd, RADEON_PARAM_IRQ_NR), param(dfd, RADEON_PARAM_LAST_FRAME));

	memset(&ctx, 0, sizeof ctx);
	if (ioctl(dfd, DRM_IOCTL_ADD_CTX, &ctx) < 0)
		die("ADD_CTX");
	lk.context = ctx.handle;
	lk.flags = 0;
	if (ioctl(dfd, DRM_IOCTL_LOCK, &lk) < 0)
		die("LOCK");
	check("lock", *lock == (ctx.handle | _DRM_LOCK_HELD));

	/* idle only if the CP reads its ring right */
	signal(SIGALRM, alarmed);
	alarm(5);
	i = ioctl(dfd, DRM_IOCTL_RADEON_CP_IDLE, 0);
	alarm(0);
	check("CP idle", i == 0);

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			fb[y * w + x] = y < h / 4 ? 0xf800 : 0x0000;
	rs->nbox = 1;
	rs->boxes[0].x1 = w / 4;
	rs->boxes[0].y1 = h * 3 / 8;
	rs->boxes[0].x2 = w * 3 / 4;
	rs->boxes[0].y2 = h * 5 / 8;
	memset(&cl, 0, sizeof cl);
	memset(&rect, 0, sizeof rect);
	cl.flags = RADEON_FRONT;
	cl.clear_color = 0x07e0;
	cl.color_mask = 0xffffffff;
	cl.depth_boxes = &rect;
	alarm(5);
	i = ioctl(dfd, DRM_IOCTL_RADEON_CLEAR, &cl) == 0 &&
	    ioctl(dfd, DRM_IOCTL_RADEON_CP_IDLE, 0) == 0;
	alarm(0);
	printf("radeontest: centre pixel 0x%04x, last clear %d\n", fb[(h / 2) * w + w / 2],
	    param(dfd, RADEON_PARAM_LAST_CLEAR));
	check("green clear through the ring", i && fb[(h / 2) * w + w / 2] == 0x07e0);

	for (i = 0; i < sizeof tex; i++)
		tex[i] = i * 7 + 1;
	memset(&tx, 0, sizeof tx);
	memset(&im, 0, sizeof im);
	im.width = TW;
	im.height = TH;
	im.data = tex;
	tx.offset = lp->textureOffset;
	tx.pitch = TW * 4 / 64;
	tx.format = TXFORMAT_ARGB8888;
	tx.width = TW;
	tx.height = TH;
	tx.image = &im;
	alarm(5);
	i = ioctl(dfd, DRM_IOCTL_RADEON_TEXTURE, &tx) == 0 &&
	    ioctl(dfd, DRM_IOCTL_RADEON_CP_IDLE, 0) == 0;
	alarm(0);
	/* VRAM should hold the bytes as sent; the 16-bit aperture swap shows them pairwise exchanged */
	vt = map(dfd, fbm.offset + lp->textureOffset, 4096UL);
	for (x = 0; i && x < sizeof tex; x++)
		if (vt[x] != tex[x ^ 1])
			break;
	printf("radeontest: texture bytes %02x %02x %02x %02x (sent %02x %02x %02x %02x)\n",
	    vt[0], vt[1], vt[2], vt[3], tex[0], tex[1], tex[2], tex[3]);
	check("texture upload through a DMA buffer", i && x == sizeof tex);

	if (param(dfd, RADEON_PARAM_IRQ_NR) > 0) {
		ie.irq_seq = &seq;
		alarm(5);
		i = ioctl(dfd, DRM_IOCTL_RADEON_IRQ_EMIT, &ie) == 0 &&
		    (iw.irq_seq = seq, ioctl(dfd, DRM_IOCTL_RADEON_IRQ_WAIT, &iw) == 0);
		alarm(0);
		check("interrupt", i);
	} else
		printf("radeontest: no interrupt, clients poll\n");

	if (ioctl(dfd, DRM_IOCTL_UNLOCK, &lk) < 0)
		die("UNLOCK");
	if (ioctl(fd, FBIOSWITCH, 0) < 0)
		die("FBIOSWITCH");
	alarm(3);
	i = ioctl(dfd, DRM_IOCTL_LOCK, &lk);
	alarm(0);
	check("no lock while hidden", i < 0 && errno == EINTR);
	if (ioctl(fd, FBIOSWITCH, (int)b.session) < 0)
		die("FBIOSWITCH");
	if (i == 0 && ioctl(dfd, DRM_IOCTL_UNLOCK, &lk) < 0)
		die("UNLOCK");
	printf("radeontest: %s\n", fails ? "FAILED" : "ok");
	fflush(stdout);
	sleep(secs);
	return fails != 0;
}
