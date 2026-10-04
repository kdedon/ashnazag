/*
 * tvst.c -- host tests of the virtual-station switch (snvst.c) against a
 * model of the rest of the driver: CAM references, host streams, wire.
 */

#include "snvsthost.h"
#include "vstation.h"

extern int sn_vinput(), sn_vlocal();

struct sn_softc sn_sc;
static int camref[16], txroom = 1, nwire, nhost, wirelen;
static unsigned char wire[1600], txb[1600];
static int nrx[8], rxlen[8];
static int fails, checks;

#define	CHECK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

int sn_spl() { return 0; }
void sn_splx(s) int s; { (void)s; }
int sn_vup() { return 1; }
int sn_txroom(sc) struct sn_softc *sc; { (void)sc; return txroom; }
unsigned char *sn_txbuf(sc) struct sn_softc *sc; { (void)sc; return txb; }
void sn_txstart(sc, len) struct sn_softc *sc; int len; { (void)sc; nwire++; wirelen = len; memcpy(wire, txb, (size_t)len); }

int
sn_camref(a, d)
	unsigned char *a;
	int d;
{
	int k, fr = -1;

	for (k = 1; k < 16; k++) {
		if (camref[k] && memcmp(sn_sc.cam[k], a, 6) == 0)
			break;
		if (!camref[k] && fr < 0)
			fr = k;
	}
	if (d < 0) {
		if (k < 16)
			camref[k]--;
		return 0;
	}
	if (k == 16) {
		if (fr < 0)
			return ENOSPC;
		k = fr;
		memcpy(sn_sc.cam[k], a, 6);
	}
	camref[k]++;
	return 0;
}

static int
camrefs()
{
	int k, n = 0;

	for (k = 1; k < 16; k++)
		n += camref[k];
	return n;
}

void
sn_input(sc, pkt, len, st)
	struct sn_softc *sc;
	unsigned char *pkt;
	int len, st;
{
	(void)sc; (void)st; (void)len;
	if (sn_vinput(pkt, len))
		return;
	nhost++;
}

static void
rx(arg, pkt, len)
	char *arg;
	unsigned char *pkt;
	int len;
{
	int i = (int)(long)arg;

	(void)pkt;
	nrx[i]++;
	rxlen[i] = len;
}

static unsigned char A[6] = { 2, 0, 0, 0, 0, 1 }, B[6] = { 2, 0, 0, 0, 0, 2 };
static unsigned char H[6] = { 8, 0, 7, 1, 2, 3 }, X[6] = { 0, 5, 2, 9, 9, 9 };
static unsigned char BC[6] = { 255, 255, 255, 255, 255, 255 };
static unsigned char MC[6] = { 9, 0, 7, 255, 255, 255 };

static unsigned char *
frame(dst, src, len)
	unsigned char *dst, *src;
	int len;
{
	static unsigned char f[1600];

	memset(f, 0xee, sizeof f);
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	f[12] = 8; f[13] = 6;
	(void)len;
	return f;
}

static void
reset()
{
	memset(nrx, 0, sizeof nrx);
	nwire = nhost = 0;
}

int
main()
{
	struct vst_ops *o = vst_ops;
	int a, b, c, d, e;
	unsigned char m[6];

	memcpy(sn_sc.cam[0], H, 6);
	a = (*o->vs_attach)(A, rx, (char *)0);
	CHECK(a >= 0, "attach A");
	CHECK((*o->vs_attach)(A, rx, (char *)1) == -EBUSY, "same address twice");
	CHECK((*o->vs_attach)(H, rx, (char *)1) == -EBUSY, "the host's address");
	CHECK((*o->vs_attach)(MC, rx, (char *)1) == -EINVAL, "a multicast address");
	b = (*o->vs_attach)(B, rx, (char *)1);
	CHECK(b >= 0 && b != a, "attach B");
	CHECK(camrefs() == 2, "a CAM entry per station");
	(*o->vs_hwaddr)(m);
	CHECK(memcmp(m, H, 6) == 0, "host address");

	reset();
	CHECK(sn_vinput(frame(A, X, 60), 60) == 1 && nrx[0] == 1 && nrx[1] == 0,
	    "wire unicast reaches only A, not the host");
	reset();
	CHECK(sn_vinput(frame(BC, X, 60), 60) == 0 && nrx[0] == 1 && nrx[1] == 1,
	    "wire broadcast reaches both and the host");
	reset();
	CHECK(sn_vinput(frame(MC, X, 60), 60) == 0 && nrx[0] == 0, "multicast off");
	CHECK((*o->vs_mcast)(a, MC, 1) == 0 && camrefs() == 3, "multicast on");
	CHECK((*o->vs_mcast)(a, BC, 1) == EINVAL, "broadcast is not a multicast");
	reset();
	CHECK(sn_vinput(frame(MC, X, 60), 60) == 0 && nrx[0] == 1 && nrx[1] == 0,
	    "multicast reaches the station that wants it");
	reset();
	CHECK(sn_vinput(frame(H, X, 60), 60) == 0 && nrx[0] + nrx[1] == 0,
	    "host unicast is not a station's");

	reset();
	CHECK(sn_vlocal(frame(A, H, 60), 60) == 1 && nrx[0] == 1, "host to A stays local");
	reset();
	CHECK(sn_vlocal(frame(BC, H, 60), 60) == 0 && nrx[0] == 1 && nrx[1] == 1,
	    "host broadcast: stations and the wire");
	reset();
	CHECK(sn_vlocal(frame(X, H, 60), 60) == 0 && nrx[0] + nrx[1] == 0, "host to the LAN");

	reset();
	CHECK((*o->vs_xmit)(a, frame(X, B, 60), 60) == EINVAL && nwire == 0,
	    "a station sends only as itself");
	CHECK((*o->vs_xmit)(a, frame(X, A, 60), 13) == EINVAL, "runt");
	CHECK((*o->vs_xmit)(a, frame(X, A, 60), 1515) == EINVAL, "giant");
	reset();
	CHECK((*o->vs_xmit)(a, frame(H, A, 42), 42) == 0 && nhost == 1 && nwire == 0 &&
	    nrx[0] == 0, "A to the host stays local");
	reset();
	CHECK((*o->vs_xmit)(a, frame(BC, A, 42), 42) == 0 && nhost == 1 && nwire == 1 &&
	    nrx[0] == 0 && nrx[1] == 1 && wirelen == 60, "A's broadcast: host, B, wire (padded)");
	reset();
	CHECK((*o->vs_xmit)(a, frame(B, A, 100), 100) == 0 && nrx[1] == 1 && rxlen[1] == 100 &&
	    nwire == 0 && nhost == 0, "A to B stays local");
	reset();
	CHECK((*o->vs_xmit)(a, frame(X, A, 100), 100) == 0 && nwire == 1 && wirelen == 100 &&
	    memcmp(wire, X, 6) == 0 && nhost == 0, "A to the LAN");
	reset();
	CHECK((*o->vs_xmit)(a, frame((unsigned char *)"\1\200\302\0\0\1", A, 60), 60) == EINVAL &&
	    nwire == 0, "pause frames refused");
	CHECK((*o->vs_xmit)(a, frame((unsigned char *)"\1\200\302\0\0\0", A, 60), 60) == EINVAL,
	    "bridge group address refused");
	CHECK((*o->vs_xmit)(a, frame((unsigned char *)"\1\200\302\0\0\40", A, 60), 60) == 0,
	    "other 01:80:c2 addresses pass");
	{
		unsigned char *f = frame(X, A, 60);

		f[12] = 0x88; f[13] = 0x08;
		CHECK((*o->vs_xmit)(a, f, 60) == EINVAL, "MAC control type refused");
	}
	CHECK((*o->vs_room)() == 1, "room");
	txroom = 0;
	CHECK((*o->vs_room)() == 0, "no room");
	CHECK((*o->vs_xmit)(a, frame(X, A, 100), 100) == EAGAIN, "no transmit room");
	txroom = 1;

	c = (*o->vs_attach)((unsigned char *)"\2\0\0\0\0\3", rx, (char *)2);
	d = (*o->vs_attach)((unsigned char *)"\2\0\0\0\0\4", rx, (char *)3);
	e = (*o->vs_attach)((unsigned char *)"\2\0\0\0\0\5", rx, (char *)4);
	CHECK(c >= 0 && d >= 0 && e == -ENOSPC, "four stations at most");
	(*o->vs_detach)(c);
	(*o->vs_detach)(d);
	(*o->vs_detach)(a);
	CHECK(camrefs() == 1, "detach drops the station's and its multicast references");
	reset();
	CHECK(sn_vinput(frame(A, X, 60), 60) == 0 && nrx[0] == 0, "detached station gets nothing");
	CHECK((*o->vs_xmit)(a, frame(X, A, 60), 60) == ENXIO, "detached station cannot send");
	(*o->vs_detach)(b);
	CHECK(camrefs() == 0, "all references gone");
	{
		static unsigned char ma[6] = { 9, 0, 7, 0, 0, 0 };
		int s1, k, e1 = 0;

		s1 = (*o->vs_attach)(A, rx, (char *)0);
		c = (*o->vs_attach)(B, rx, (char *)1);
		d = (*o->vs_attach)((unsigned char *)"\2\0\0\0\0\3", rx, (char *)2);
		for (k = 0; k < 8 && e1 == 0; k++) {
			ma[5] = k;
			e1 = (*o->vs_mcast)(k < 4 ? s1 : c, ma, 1);
		}
		CHECK(e1 == ENOSPC && camrefs() == 9 && k == 7, "stations hold at most 9 CAM entries");
		CHECK((*o->vs_attach)((unsigned char *)"\2\0\0\0\0\4", rx, (char *)3) == -ENOSPC,
		    "no station beyond the CAM share");
		(*o->vs_detach)(s1);
		(*o->vs_detach)(c);
		(*o->vs_detach)(d);
		CHECK(camrefs() == 0, "CAM share returned");
	}

	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
