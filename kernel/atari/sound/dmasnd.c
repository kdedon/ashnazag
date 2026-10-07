/*
 * dmasnd.c -- /dev/dmasnd: the Falcon's (or TT's) DMA sound for the sound
 * service.
 *
 * The DMA plays a ring in ST-RAM, reserved at boot, as a chain of BLOCK-byte
 * frames: the hardware latches a frame's addresses at the end of the one
 * playing, and each frame's end is an MFP input 7 interrupt that queues the
 * block after next.  So the DMA walks the ring in order.  A clock tick, only
 * while playing, reads the frame counter, zeroes what has played (a writer
 * that falls behind leaves silence, not old samples) and wakes the writer.
 * Past the queued data the next write starts a lead ahead of the DMA, that
 * lead being silence.  A late interrupt plays a block twice; the tick sees
 * the counter go back and waits the replay out as silence.  With no
 * interrupt for two blocks' time the ring becomes one repeating frame.
 * A ring's worth of silence stops the DMA, so idle costs nothing.
 *
 * Falcon: 16-bit stereo through the crossbar to the codec at 25.175 MHz /
 * 256 / (prescale + 1), or 8-bit at the STE rates.  TT and STE: 8-bit,
 * with the LMC1992 set over MICROWIRE, waiting for each command.
 *
 * While a passthrough guest is in front it owns the hardware: the DMA
 * stops, queued data is dropped and writes fail EBUSY until it leaves.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/cred.h"
#include "sys/uio.h"
#include "sys/file.h"
#include "ds.h"
#include "sndio.h"

#define REG(o)		(*(VOL unsigned char *)(0xFFFF8900UL + (o)))
#define REGW(o)		(*(VOL unsigned short *)(0xFFFF8900UL + (o)))
#define R_IRQ		0x00		/* Falcon: bit 0, input 7 at play's frame ends */
#define R_CTRL		0x01		/* 1 play, 2 repeat */
#define R_BASE		0x03		/* high, mid (+2), low (+4) */
#define R_COUNT		0x09
#define R_END		0x0F
#define R_TRACKS	0x20
#define R_MODE		0x21		/* 0x80 mono, 0x40 16-bit; STE rate in bits 1-0 */
#define R_MWDATA	0x22
#define R_MWMASK	0x24
#define R_SRC		0x30		/* crossbar source and destination words */
#define R_DST		0x32
#define R_PRESCALE	0x35		/* 25.175 MHz divider; 0 STE rates */
#define R_ADDER		0x37
#define R_ADCIN		0x38
#define R_ATTEN		0x3A
#define GPIP		(*(VOL unsigned char *)0xFFFFFA01UL)

#define RING		0x10000		/* = SNDBUF in the platform code */
#define BLOCK		0x1000		/* one DMA frame of the chain */
#define BOUNCE		1024		/* write() moves this much per step */
#define TICKUS		10000
#define PRI		((PZERO + 1) | PCATCH)
#define USED(a, b)	(((b) - (a)) & (RING - 1))	/* bytes from a to b */

extern unsigned long ata_sndbuf, ata_mch;
extern void ata_sirq();
extern int uiomove(), copyin(), copyout(), timeout(), drv_usectohz();
extern void untimeout(), pollwakeup(), drv_usecwait(), bzero(), bcopy();

static struct {
	int	open, falcon, held;
	caddr_t	ring;			/* physical = kernel address */
	int	mode, prescale, fsize;	/* R_MODE, R_PRESCALE, bytes per frame */
	long	bps;			/* bytes per second */
	int	limit;			/* most bytes queued */
	int	vol;
	int	playing, chained;
	int	rise;			/* frame ends are rising edges at input 7 */
	int	wpos;			/* next byte write() fills */
	int	cpos;			/* play position as of the last tick */
	int	filled;			/* bytes written, not yet played */
	int	gap;			/* silence ahead of them */
	int	lead;
	int	idle;			/* silence played since the data ran out */
	int	nextblk;		/* the block the DMA plays next */
	int	unchain_at;		/* unchaining: the frame that ends the chain starts here */
	unsigned long lastintrs;
	int	quiet, quietmax;	/* ticks without a frame interrupt; when it is lost */
	int	tid, tick;
	struct dmastat st;
} snd;

static struct sndev snd_ev;
static int snd_pend;
static struct pollhead snd_ph;

/* Falcon prescales and their rates; the STE (and TT) rates */
static short fpre[] = { 1, 2, 3, 4, 5, 7, 9, 11 };
static long frate[] = { 49170, 32780, 24585, 19668, 16390, 12292, 9834, 8195 };
static long srate[] = { 6258, 12517, 25033, 50066 };
static char mwvol[] = { 0, 6, 11, 17, 23, 29, 34, 40 };	/* LMC1992 master, 2 dB steps */

static int
nearest(r, t, n)
long r, *t;
int n;
{
	register int i, best = 0;

	for (i = 1; i < n; i++)
		if ((r > t[i] ? r - t[i] : t[i] - r) < (r > t[best] ? r - t[best] : t[best] - r))
			best = i;
	return best;
}

static void
snd_event(bits)
int bits;
{
	snd_ev.se_irq |= bits;
	snd_pend = 1;
	wakeup((caddr_t)&snd_ev);
	pollwakeup(&snd_ph, POLLIN | POLLRDNORM);
}

static void
snd_room()
{
	wakeup((caddr_t)&snd);
	pollwakeup(&snd_ph, POLLOUT | POLLWRNORM);
}

/* the frame counter as a ring offset; two reads must agree, as it moves */
static int
snd_ppos()
{
	register unsigned long a, b;

	do {
		a = (unsigned long)REG(R_COUNT) << 16 | REG(R_COUNT + 2) << 8 | REG(R_COUNT + 4);
		b = (unsigned long)REG(R_COUNT) << 16 | REG(R_COUNT + 2) << 8 | REG(R_COUNT + 4);
	} while (a != b);
	return (int)((a - (unsigned long)snd.ring) & (RING - 1));
}

static void
snd_setaddr(reg, a)
int reg;
unsigned long a;
{
	REG(reg) = a >> 16;
	REG(reg + 2) = a >> 8;
	REG(reg + 4) = a & 0xFE;
}

/* one LMC1992 command: the mask rotates while it shifts out, at least 16 us */
static void
snd_mw(cmd)
int cmd;
{
	register int i;

	REGW(R_MWMASK) = 0x7FF;
	REGW(R_MWDATA) = cmd;
	for (i = 0; i < 1000 && REGW(R_MWMASK) == 0x7FF; i++)
		;
	for (i = 0; i < 100000 && REGW(R_MWMASK) != 0x7FF; i++)
		;
	drv_usecwait(30);
}

static void
snd_volume()
{
	int a;

	if (snd.falcon) {
		a = snd.vol ? (7 - snd.vol) * 2 : 15;
		REGW(R_ATTEN) = a << 8 | a << 4;
	} else
		snd_mw(0x4C0 | mwvol[snd.vol]);
}

/* TT/STE: the LMC1992 as the host plays */
static void
snd_mwinit()
{
	if (snd.falcon)
		return;
	snd_mw(0x401);		/* the PSG mixed in */
	snd_volume();
	snd_mw(0x554);		/* left, right: 0 dB */
	snd_mw(0x514);
	snd_mw(0x486);		/* treble, bass: flat */
	snd_mw(0x446);
}

/* n bytes of the ring from o */
static void
snd_zero(o, n)
int o, n;
{
	if (o + n <= RING)
		bzero(snd.ring + o, n);
	else {
		bzero(snd.ring + o, RING - o);
		bzero(snd.ring, n - (RING - o));
	}
}

/* data restarts the lead ahead of the DMA, or of the ring's start */
static void
snd_reset()
{
	snd.wpos = snd.gap = snd.lead;
	snd.filled = 0;
}

/* the frame interrupt (input 7, IPL 6): queue the block after next */
static void
snd_intr()
{
	register int p, nb;

	if (!snd.playing || !snd.chained)
		return;
	snd.st.d_intrs++;
	p = snd_ppos();
	if (USED(snd.nextblk, p) >= BLOCK) {	/* a frame went by unseen */
		snd.st.d_skips++;
		snd.nextblk = p & ~(BLOCK - 1);
	}
	nb = (snd.nextblk + BLOCK) & (RING - 1);
	snd_setaddr(R_BASE, (unsigned long)snd.ring + nb);
	snd_setaddr(R_END, (unsigned long)snd.ring + nb + BLOCK);
	snd.nextblk = nb;
}

/* DMA off, the ring silent; at DS_HI */
static void
snd_stop()
{
	REG(R_CTRL) = 0;
	if (snd.chained)
		ata_sirq(0, 0);
	snd.playing = snd.chained = snd.unchain_at = 0;
	if (snd.tid)
		untimeout(snd.tid);
	snd.tid = 0;
	if (snd.filled)
		snd_event(DMA_EVEMPTY);
	snd_zero(snd.cpos, USED(snd.cpos, snd.wpos));
	snd_reset();
}

/* The frame interrupt is lost: the DMA repeats block nextblk.  Its next
 * frame runs from the block after it to the end of the ring; once that
 * has started, the tick makes the whole ring the repeating frame. */
static void
snd_unchain()
{
	int nb = (snd.nextblk + BLOCK) & (RING - 1);

	snd.chained = 0;
	ata_sirq(0, 0);
	snd_setaddr(R_BASE, (unsigned long)snd.ring + nb);
	snd_setaddr(R_END, (unsigned long)snd.ring + RING);
	snd.unchain_at = nb;
}

static void
snd_tick()
{
	register int p, n, x, o;

	x = DS_SPL(DS_HI);
	snd.tid = 0;
	if (!snd.playing) {
		DS_SPLX(x);
		return;
	}
	p = snd_ppos();
	p -= p % snd.fsize;		/* a frame half played is kept whole */
	/* a block played again: the replayed bytes are silence to wait out */
	if ((snd.chained || snd.unchain_at) && USED(snd.cpos, p) > RING / 2) {
		snd.st.d_repeats++;
		snd.gap += USED(p, snd.cpos);
		snd.cpos = p;
	}
	if (snd.unchain_at && p >= snd.unchain_at) {	/* the last frame has begun */
		snd_setaddr(R_BASE, (unsigned long)snd.ring);
		snd_setaddr(R_END, (unsigned long)snd.ring + RING);
		snd.unchain_at = 0;
	}
	n = USED(snd.cpos, p);
	o = snd.cpos;
	DS_SPLX(x);
	snd_zero(o, n);
	x = DS_SPL(DS_HI);
	if (!snd.playing || snd.cpos != o) {
		DS_SPLX(x);
		return;
	}
	if (n > snd.gap + snd.filled) {		/* played past the data */
		if (snd.filled) {
			snd.st.d_under++;
			snd.st.d_played += snd.filled;
			snd_event(DMA_EVEMPTY);
		}
		snd.idle += n - snd.gap - snd.filled;
		snd.wpos = (p + snd.lead) & (RING - 1) & ~3;
		snd.gap = USED(p, snd.wpos);
		snd.filled = 0;
	} else if (n > snd.gap) {
		snd.filled -= n - snd.gap;
		snd.st.d_played += n - snd.gap;
		snd.gap = 0;
		if (snd.filled == 0)
			snd_event(DMA_EVEMPTY);
	} else
		snd.gap -= n;
	snd.cpos = p;
	if (snd.chained) {
		if (snd.st.d_intrs != snd.lastintrs) {
			snd.lastintrs = snd.st.d_intrs;
			snd.quiet = 0;
		} else if (++snd.quiet > snd.quietmax) {
			snd.st.d_fallbacks++;
			snd_unchain();
		}
	}
	if (snd.filled == 0 && snd.idle >= RING)
		snd_stop();
	else
		snd.tid = timeout(snd_tick, (caddr_t)0, snd.tick);
	DS_SPLX(x);
	snd_room();
}

/* the DMA from the start of the ring, chained; at DS_HI */
static void
snd_start()
{
	REG(R_CTRL) = 0;
	if (snd.falcon) {
		REG(R_TRACKS) = 0;
		REGW(R_SRC) = (REGW(R_SRC) & 0xFFF0) | 0x9;	/* DMA play: 25 MHz, no handshake */
		REGW(R_DST) &= 0x9FFF;				/* the DAC hears the DMA */
		REG(R_PRESCALE) = snd.prescale;
		REG(R_ADDER) = 3;
		REG(R_ADCIN) = 3;
		REG(R_IRQ) = 1;
		snd_volume();
	}
	REG(R_MODE) = snd.mode;
	/* the line's idle level: frame ends go back to it */
	snd.rise = (GPIP & 0x80) != 0;
	snd.cpos = 0;
	snd.idle = 0;
	snd.unchain_at = 0;
	snd.quiet = 0;
	snd.lastintrs = snd.st.d_intrs;
	snd.quietmax = (int)(2L * BLOCK * (1000000 / TICKUS) / snd.bps) + 2;
	snd_setaddr(R_BASE, (unsigned long)snd.ring);
	snd_setaddr(R_END, (unsigned long)snd.ring + BLOCK);
	REG(R_CTRL) = 3;
	snd_setaddr(R_BASE, (unsigned long)snd.ring + BLOCK);
	snd_setaddr(R_END, (unsigned long)snd.ring + 2 * BLOCK);
	snd.nextblk = BLOCK;
	snd.chained = snd.playing = 1;
	ata_sirq(1, snd.rise);
	snd.st.d_starts++;
	snd.tid = timeout(snd_tick, (caddr_t)0, snd.tick);
}

static int
snd_setfmt(f)
register struct dmafmt *f;
{
	register int i;

	if (!snd.falcon || f->d_bits != 16) {
		f->d_bits = 8;
		/* the Falcon's DAC is mute at the lowest STE rate */
		i = snd.falcon + nearest(f->d_rate, srate + snd.falcon, 4 - snd.falcon);
		f->d_rate = srate[i];
		f->d_chans = f->d_chans == 1 ? 1 : 2;
		snd.mode = (f->d_chans == 1 ? 0x80 : 0) | i;
		snd.prescale = 0;
	} else {
		i = nearest(f->d_rate, frate, 8);
		f->d_rate = frate[i];
		f->d_chans = 2;
		snd.mode = 0x40;
		snd.prescale = fpre[i];
	}
	snd.fsize = f->d_chans * f->d_bits / 8;
	snd.bps = f->d_rate * snd.fsize;
	snd.lead = (int)(snd.bps >> 6) & ~3;	/* 16 ms */
	if (snd.limit > RING - 2 * BLOCK)
		snd.limit = RING - 2 * BLOCK;
	snd_reset();
	return 0;
}

/* from a display switch, at DS_HI */
static void
snd_front(s)
struct dssess *s;
{
	snd_ev.se_front = s->s_id;
	snd_ev.se_fuid = s->s_uid;
	snd_ev.se_serial++;
	snd_event(0);
}

/* a passthrough guest takes (1) or leaves (0) the hardware */
static void
snd_own(on)
int on;
{
	register int x = DS_SPL(DS_HI);

	if (on && snd.playing)
		snd_stop();
	snd.held = snd_ev.se_hold = on;
	snd_event(0);
	DS_SPLX(x);
	if (!on)
		snd_mwinit();	/* the guest may have changed it */
	snd_room();
}

static int
snd_room_now()
{
	register int r = snd.limit - snd.gap - snd.filled;

	r -= r % snd.fsize;
	return r < 0 ? 0 : r;
}

/* poll's POLLOUT: room for a quarter of the limit, so the writer batches */
static int
snd_ready()
{
	return !snd.held && snd_room_now() >= snd.limit / 4;
}

/*ARGSUSED*/
int
dma_open(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
struct cred *cr;
{
	register int x;
	struct dmafmt f;

	if (getminor(*devp) != 0)
		return ENXIO;
	if (drv_priv(cr))
		return EPERM;
	/* the _MCH cookie, or its machine number as Linux boot info gives it */
	switch (ata_mch >= 0x10000 ? ata_mch >> 16 : ata_mch) {
	case 3: snd.falcon = 1; break;
	case 1: case 2: snd.falcon = 0; break;
	default: return ENXIO;
	}
	if (ata_sndbuf & 1)
		return ENOMEM;
	x = DS_SPL(DS_HI);
	if (snd.open) {
		DS_SPLX(x);
		return EBUSY;
	}
	snd.open = 1;
	DS_SPLX(x);
	snd.ring = (caddr_t)ata_sndbuf;
	snd.playing = snd.chained = 0;
	bzero(snd.ring, RING);
	bzero((caddr_t)&snd.st, sizeof snd.st);
	snd.tick = drv_usectohz(TICKUS);
	if (snd.tick < 1)
		snd.tick = 1;
	snd.vol = 7;
	snd.limit = RING;
	f.d_rate = snd.falcon ? 24585 : 25033;
	f.d_bits = snd.falcon ? 16 : 8;
	f.d_chans = 2;
	(void)snd_setfmt(&f);
	x = DS_SPL(DS_HI);
	snd_ev.se_irq = 0;
	snd_ev.se_front = ds_front->s_id;
	snd_ev.se_fuid = ds_front->s_uid;
	snd.held = snd_ev.se_hold = DS_GUEST(ds_front) != 0;
	if (!snd.held)
		REG(R_CTRL) = 0;
	snd_pend = 1;
	ds_frontfn = snd_front;
	ds_sndown = snd_own;
	ds_sndhw = snd_intr;
	DS_SPLX(x);
	if (!snd.held)
		snd_mwinit();
	return 0;
}

/*ARGSUSED*/
int
dma_close(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
struct cred *cr;
{
	register int x = DS_SPL(DS_HI);

	if (snd.playing)
		snd_stop();
	ds_frontfn = 0;
	ds_sndown = 0;
	ds_sndhw = 0;
	snd.open = 0;
	DS_SPLX(x);
	return 0;
}

/*ARGSUSED*/
int
dma_read(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	struct sndev e;
	register int x;

	if (uio->uio_resid < sizeof e)
		return EINVAL;
	x = DS_SPL(DS_HI);
	while (!snd_pend)
		if (sleep((caddr_t)&snd_ev, PRI)) {
			DS_SPLX(x);
			return EINTR;
		}
	e = snd_ev;
	snd_ev.se_irq = 0;
	snd_ev.se_nirq = snd.st.d_intrs;
	snd_pend = 0;
	DS_SPLX(x);
	e.se_nirq = snd.st.d_intrs;
	return uiomove((caddr_t)&e, sizeof e, UIO_READ, uio);
}

/*ARGSUSED*/
int
dma_write(dev, uio, cr)
dev_t dev;
struct uio *uio;
struct cred *cr;
{
	char buf[BOUNCE];
	register int n, i, chunk, x;
	int resid = uio->uio_resid;

	while (uio->uio_resid >= snd.fsize) {
		x = DS_SPL(DS_HI);
		while ((n = snd_room_now()) == 0 && !snd.held) {
			if (uio->uio_fmode & (FNDELAY | FNONBLOCK)) {
				DS_SPLX(x);
				return uio->uio_resid == resid ? EAGAIN : 0;
			}
			if (sleep((caddr_t)&snd, PRI)) {
				DS_SPLX(x);
				return EINTR;
			}
		}
		DS_SPLX(x);
		if (snd.held)
			return EBUSY;
		if (n > BOUNCE)
			n = BOUNCE;
		if (n > uio->uio_resid)
			n = uio->uio_resid - uio->uio_resid % snd.fsize;
		if (uiomove(buf, (long)n, UIO_WRITE, uio))
			return EFAULT;
		x = DS_SPL(DS_HI);
		if (snd.held) {
			DS_SPLX(x);
			return EBUSY;
		}
		for (i = 0; i < n; i += chunk) {
			chunk = RING - snd.wpos < n - i ? RING - snd.wpos : n - i;
			bcopy(buf + i, snd.ring + snd.wpos, chunk);
			snd.wpos = (snd.wpos + chunk) & (RING - 1);
		}
		snd.filled += n;
		snd.idle = 0;
		if (!snd.playing)
			snd_start();
		DS_SPLX(x);
	}
	return 0;
}

/*ARGSUSED*/
int
dma_ioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd, arg, mode;
struct cred *cr;
int *rvalp;
{
	struct dmafmt f;
	register int x;

	switch (cmd) {
	case DMA_SETFMT:
		if (copyin((caddr_t)arg, (caddr_t)&f, sizeof f))
			return EFAULT;
		x = DS_SPL(DS_HI);
		if (snd.playing)
			snd_stop();
		(void)snd_setfmt(&f);
		DS_SPLX(x);
		return copyout((caddr_t)&f, (caddr_t)arg, sizeof f) ? EFAULT : 0;
	case DMA_GETDELAY:
		*rvalp = snd.gap + snd.filled;
		return 0;
	case DMA_SETLIMIT:
		if (arg < BLOCK || arg > RING - 2 * BLOCK)
			arg = arg < BLOCK ? BLOCK : RING - 2 * BLOCK;
		snd.limit = arg;
		return 0;
	case DMA_SETVOL:
		if (arg < 0 || arg > 7)
			return EINVAL;
		snd.vol = arg;
		if (!snd.held)
			snd_volume();
		return 0;
	case DMA_FLUSH:
		x = DS_SPL(DS_HI);
		if (snd.playing)
			snd_stop();
		snd_reset();
		DS_SPLX(x);
		snd_room();
		return 0;
	case DMA_STATS:
		snd.st.d_chained = snd.chained;
		snd.st.d_playing = snd.playing;
		snd.st.d_held = snd.held;
		return copyout((caddr_t)&snd.st, (caddr_t)arg, sizeof snd.st) ? EFAULT : 0;
	}
	return EINVAL;
}

/*ARGSUSED*/
int
dma_poll(dev, events, anyyet, reventsp, phpp)
dev_t dev;
short events;
int anyyet;
short *reventsp;
struct pollhead **phpp;
{
	register short r = 0;

	if (snd_pend)
		r |= events & (POLLIN | POLLRDNORM);
	if (snd_ready())
		r |= events & (POLLOUT | POLLWRNORM);
	*reventsp = r;
	if (r == 0 && !anyyet)
		*phpp = &snd_ph;
	return 0;
}
