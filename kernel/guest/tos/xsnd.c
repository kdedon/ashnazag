/*
 * xsnd.c -- the Falcon XBIOS sound calls, in the cartridge.  They keep
 * the play state in struct tossnd and ring the sound pump, a process of
 * starttos that streams the guest's buffer to the host's sound service
 * and raises the buffer-end interrupts.  Recording is absent: its
 * buffer is accepted, never filled.
 */

#include "tosio.h"

extern long sys_write();
extern char xb[];
extern long old_xb;

#define	SD	((struct tossnd *)TOSSND)

static int locked;
/* Soundcmd: attenuations, gains, adder input, ADC input, STE prescale */
static short scmd[7] = { 0, 0, 0, 0, 1, 3, 1 };
static short steclk[4] = { 6258, 12517, 25033, 50066 };
static int dclk, dpre;			/* Devconnect's clock and prescale */

#define	L(a, i)	((unsigned long)(a)[i] << 16 | (a)[(i) + 1])

static void
ring()
{
	char b = 1;

	SD->sd_gen++;
	sys_write(SD->sd_bell, &b, 1L);
}

/* the play rate: clock (25.175 MHz, external 22.5792 MHz, STE) over 256 (prescale + 1) */
static void
rate(clk, pre)
	int clk, pre;
{
	static unsigned long hz[2] = { 25175000, 22579200 };

	dclk = clk;
	dpre = pre;
	if (clk >= 2 || pre == 0)
		SD->sd_rate = (unsigned long)steclk[scmd[6] & 3] << 16;
	else
		SD->sd_rate = hz[clk] / (pre + 1) << 8;
}

/* the defaults of a reset: stereo 8-bit, one track, the DAC at 49170 Hz */
static void
reset()
{
	SD->sd_ctl = 0;
	SD->sd_mode = 0;
	SD->sd_tracks = 0;
	SD->sd_mon = 0;
	SD->sd_irq = 0;
	rate(0, 1);
}

/* XBIOS a[0]; 1 when it is ours, its result in *rv */
long
xsnd(a, rv)
	unsigned short *a;
	long *rv;
{
	long r = 0, *p;
	int n;

	switch (a[0]) {
	case 128:			/* Locksnd */
		r = locked ? -129 : (locked = 1);
		break;
	case 129:			/* Unlocksnd */
		r = locked ? (locked = 0) : -128;
		break;
	case 130:			/* Soundcmd(mode, data) */
		if (a[1] > 6)
			return 0;
		if ((short)a[2] >= 0) {
			scmd[a[1]] = a[2];
			if (a[1] == 6) {
				rate(dclk, dpre);
				ring();
			}
		}
		r = scmd[a[1]];
		break;
	case 131:			/* Setbuffer(reg, beg, end) */
		if (a[1] == 0) {
			SD->sd_beg = L(a, 2);
			SD->sd_end = L(a, 4);
			ring();
		}
		break;
	case 132:			/* Setmode */
		if (a[1] > 2)
			r = -1;
		else {
			SD->sd_mode = a[1];
			ring();
		}
		break;
	case 133:			/* Settracks(play, record) */
		SD->sd_tracks = a[1] & 3;
		ring();
		break;
	case 134:			/* Setmontracks */
		SD->sd_mon = a[1] & 3;
		ring();
		break;
	case 135:			/* Setinterrupt(source, cause): play ends only */
		n = a[1] ? TSE_GPIP7 : TSE_TIMERA;
		SD->sd_irq = a[2] & 1 ? SD->sd_irq | n : SD->sd_irq & ~n;
		break;
	case 136:			/* Buffoper */
		if ((short)a[1] < 0) {
			r = SD->sd_ctl & SB_REPEAT;
			if (SD->sd_seen != SD->sd_gen ? SD->sd_ctl & SB_PLAY : SD->sd_play)
				r |= SB_PLAY;
		} else {
			SD->sd_ctl = a[1] & (SB_PLAY | SB_REPEAT);
			ring();
		}
		break;
	case 137:			/* Dsptristate */
		break;
	case 139:			/* Devconnect(src, dst, clk, prescale, protocol): DMA play */
		if (a[1] == 0) {
			rate(a[3], a[4]);
			ring();
		}
		break;
	case 140:			/* Sndstatus(reset) */
		if (a[1] == 1) {
			reset();
			ring();
		}
		break;
	case 141:			/* Buffptr(ptr) */
		p = (long *)L(a, 1);
		p[0] = SD->sd_play ? SD->sd_pos : SD->sd_beg;
		p[1] = p[2] = p[3] = 0;
		break;
	default:
		return 0;
	}
	*rv = r;
	return 1;
}

/*
 * With a pump: the calls, and the cookie's DMA, CODEC and matrix bits
 * for programs that look before they call.
 */
void
xsndinit()
{
	long *j = *(long **)0x5a0;
	int n;

	if (SD->sd_bell < 0)
		return;
	reset();
	old_xb = *(long *)0xb8;
	*(long *)0xb8 = (long)xb;
	if (!j)
		return;
	for (n = 0; j[0]; j += 2, n++)
		if (j[0] == 0x5f534e44) {	/* _SND */
			j[1] |= 0x16;
			return;
		}
	if (n + 1 < j[1]) {
		j[2] = 0;
		j[3] = j[1];
		j[0] = 0x5f534e44;
		j[1] = 0x17;
	}
}
