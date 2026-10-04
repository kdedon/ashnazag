/*
 * snvst.c -- virtual stations on the SONIC: each has its own CAM entry
 * and handler.  Unicast to a station reaches only that station; broadcast
 * and multicast reach the host's streams, every other station that wants
 * them and the wire.  A station sends only with its own source address.
 * Builds on the host with -DSN_HOST for the tests.
 *
 * K&R C.
 */

#ifdef SN_HOST
#include "snvsthost.h"
#else
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sonic.h"
#endif
#include "vstation.h"

#define	SN_NVST		4
#define	SN_VCAM		9		/* CAM entries stations may use; 6 stay the host's */

extern struct sn_softc sn_sc;
extern int sn_spl(), sn_camref(), sn_vup();
extern void sn_splx(), sn_input();
extern int bcmp();

struct sn_vst {
	int		v_inuse;
	unsigned char	v_mac[6];
	void		(*v_rx)();
	char		*v_arg;
	int		v_nmc;
	unsigned char	v_mc[VST_NMC][6];
};

struct sn_vst	sn_vst[SN_NVST];
int		sn_vquiet;		/* frame from a station going to the host */
unsigned long	sn_vstat[4];		/* local, to wire, dropped (no room), refused */
int		sn_vcam;		/* CAM references held by stations */

static int sn_vattach(), sn_vxmit(), sn_vmcast(), sn_vroom();
static void sn_vdetach(), sn_vhwaddr();

struct vst_ops	sn_vstops = { sn_vattach, sn_vdetach, sn_vxmit, sn_vmcast, sn_vhwaddr, sn_vroom };
struct vst_ops	*vst_ops = &sn_vstops;

static unsigned char sn_vbcast[6] = { 255, 255, 255, 255, 255, 255 };

static int
wants(v, a)
	struct sn_vst *v;
	unsigned char *a;
{
	int i;

	if (bcmp((char *)a, (char *)sn_vbcast, 6) == 0)
		return 1;
	for (i = 0; i < v->v_nmc; i++)
		if (bcmp((char *)a, (char *)v->v_mc[i], 6) == 0)
			return 1;
	return 0;
}

/* Hand a frame to the stations it is for, except station skip. */
static int
sn_vdeliver(pkt, len, skip)
	unsigned char *pkt;
	int len, skip;
{
	struct sn_vst *v;
	int i, n = 0;

	for (i = 0, v = sn_vst; i < SN_NVST; i++, v++) {
		if (!v->v_inuse || i == skip)
			continue;
		if (pkt[0] & 1 ? wants(v, pkt) : bcmp((char *)pkt, (char *)v->v_mac, 6) == 0) {
			(*v->v_rx)(v->v_arg, pkt, len);
			n++;
		}
	}
	return n;
}

/*
 * From the wire (sn_input): 1 if the frame was a station's unicast and
 * the host's streams should not see it.
 */
int
sn_vinput(pkt, len)
	unsigned char *pkt;
	int len;
{
	if (sn_vquiet)
		return 0;
	return sn_vdeliver(pkt, len, -1) && !(pkt[0] & 1);
}

/* From the host's streams (sn_xmit): 1 if the frame stays local. */
int
sn_vlocal(pkt, len)
	unsigned char *pkt;
	int len;
{
	if (sn_vdeliver(pkt, len, -1) && !(pkt[0] & 1)) {
		sn_vstat[0]++;
		return 1;
	}
	return 0;
}

static int
sn_vattach(mac, rx, arg)
	unsigned char *mac;
	void (*rx)();
	char *arg;
{
	struct sn_vst *v;
	int i, s, e;

	if (mac[0] & 1 || bcmp((char *)mac, "\0\0\0\0\0\0", 6) == 0)
		return -EINVAL;
	if (!sn_vup())
		return -ENXIO;
	s = sn_spl();
	if (bcmp((char *)mac, (char *)sn_sc.cam[0], 6) == 0) {
		sn_splx(s);
		return -EBUSY;
	}
	for (i = 0; i < SN_NVST; i++)
		if (sn_vst[i].v_inuse && bcmp((char *)mac, (char *)sn_vst[i].v_mac, 6) == 0) {
			sn_splx(s);
			return -EBUSY;
		}
	for (i = 0; i < SN_NVST && sn_vst[i].v_inuse; i++)
		;
	if (i == SN_NVST) {
		sn_splx(s);
		return -ENOSPC;
	}
	if (sn_vcam >= SN_VCAM) {
		sn_splx(s);
		return -ENOSPC;
	}
	if ((e = sn_camref(mac, 1)) != 0) {
		sn_splx(s);
		return -e;
	}
	sn_vcam++;
	v = &sn_vst[i];
	bcopy((char *)mac, (char *)v->v_mac, 6);
	v->v_rx = rx;
	v->v_arg = arg;
	v->v_nmc = 0;
	v->v_inuse = 1;
	sn_splx(s);
	return i;
}

static void
sn_vdetach(st)
	int st;
{
	struct sn_vst *v = &sn_vst[st];
	int s;

	if (st < 0 || st >= SN_NVST || !v->v_inuse)
		return;
	s = sn_spl();
	while (v->v_nmc > 0) {
		(void)sn_camref(v->v_mc[--v->v_nmc], -1);
		sn_vcam--;
	}
	(void)sn_camref(v->v_mac, -1);
	sn_vcam--;
	v->v_inuse = 0;
	sn_splx(s);
}

static int
sn_vmcast(st, a, on)
	int st, on;
	unsigned char *a;
{
	struct sn_vst *v = &sn_vst[st];
	int i, s, e = 0;

	if (st < 0 || st >= SN_NVST || !v->v_inuse || !(a[0] & 1) ||
	    bcmp((char *)a, (char *)sn_vbcast, 6) == 0)
		return EINVAL;
	s = sn_spl();
	for (i = 0; i < v->v_nmc; i++)
		if (bcmp((char *)a, (char *)v->v_mc[i], 6) == 0)
			break;
	if (on && i == v->v_nmc) {
		if (v->v_nmc == VST_NMC || sn_vcam >= SN_VCAM)
			e = ENOSPC;
		else if ((e = sn_camref(a, 1)) == 0) {
			bcopy((char *)a, (char *)v->v_mc[v->v_nmc++], 6);
			sn_vcam++;
		}
	} else if (!on && i < v->v_nmc) {
		(void)sn_camref(a, -1);
		sn_vcam--;
		for (v->v_nmc--; i < v->v_nmc; i++)
			bcopy((char *)v->v_mc[i + 1], (char *)v->v_mc[i], 6);
	}
	sn_splx(s);
	return e;
}

static int
sn_vxmit(st, pkt, len)
	int st, len;
	unsigned char *pkt;
{
	struct sn_vst *v = &sn_vst[st];
	unsigned char *b;
	int s, n;

	if (st < 0 || st >= SN_NVST || !v->v_inuse)
		return ENXIO;
	/* own source only; no bridge control (01:80:c2:00:00:0x) or pause frames */
	if (len < VST_MINFRAME || len > VST_MAXFRAME ||
	    bcmp((char *)pkt + 6, (char *)v->v_mac, 6) != 0 ||
	    (bcmp((char *)pkt, "\1\200\302\0\0", 5) == 0 && (pkt[5] & 0xf0) == 0) ||
	    (pkt[12] == 0x88 && pkt[13] == 0x08)) {
		sn_vstat[3]++;
		return EINVAL;
	}
	s = sn_spl();
	n = sn_vdeliver(pkt, len, st);
	if (pkt[0] & 1 || bcmp((char *)pkt, (char *)sn_sc.cam[0], 6) == 0) {
		sn_vquiet++;
		sn_input(&sn_sc, pkt, len, 0);
		sn_vquiet--;
		n++;
	}
	if (n && !(pkt[0] & 1)) {
		sn_vstat[0]++;
		sn_splx(s);
		return 0;
	}
	if (!sn_txroom(&sn_sc)) {
		sn_vstat[2]++;
		sn_splx(s);
		return EAGAIN;
	}
	b = sn_txbuf(&sn_sc);
	bcopy((char *)pkt, (char *)b, len);
	for (; len < 60; len++)
		b[len] = 0;
	sn_txstart(&sn_sc, len);
	sn_vstat[1]++;
	sn_splx(s);
	return 0;
}

static int
sn_vroom()
{
	return sn_txroom(&sn_sc);
}

static void
sn_vhwaddr(mac)
	unsigned char *mac;
{
	bcopy((char *)sn_sc.cam[0], (char *)mac, 6);
}
