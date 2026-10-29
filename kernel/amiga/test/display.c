#include "../amigads.c"
#include "../mistrtg/mistrtg.c"
extern void *malloc(), *calloc();
extern void free();
extern int printf();
unsigned int displaytype;
struct dssess ds_sess[1 + DS_NSESS], *ds_front;
struct dsdisp ds_disp;
void (*ds_frontfn)();
static struct screen native, service;
struct screen *activescreen = &native;
static int allocno, failat, live, openfail, selected, displayed, frames;
static int mx, my, packets, bounds;
static int setn, sett, setb;
static void tset(n, bits) int n, bits; { setn++; sett = n; setb = bits; }
static void tnop() { }
static struct am_mode tmodes[] = { { 640, 480, 60 }, { 800, 600, 60 }, { 1024, 768, 60 }, { 1280, 1024, 60 } };
static struct am_rtg trtg;
/* the MiSTer RTG board: an autoconfig entry, its window and registers as memory */
static char *mrmem;
int mr_width = 1280, mr_height = 1024;
int plat_zorro(i, zb) int i; struct amx_zboard *zb;
{
	if (i > 1) return 1;
	zb->zb_manuf = 0x139C; zb->zb_prod = i ? 0x30 : 0x10;
	zb->zb_pa = i ? 0x50000000UL : 0x40000000UL; zb->zb_size = 0x1000000UL;
	return 0;
}
char *plat_iomap(pa, size) unsigned long pa, size; { return pa == 0x50000000UL && size <= 0x1000000UL ? mrmem : 0; }
#define MREG(o)	(*(unsigned short *)(mrmem + MR_REGS + (o)))
/* the mode an environment's rule picks: deepest packed mode up to 8 bits at the session's size */
static int envpick(w, h) int w, h;
{
	struct fbinfo fi;
	int n, best = -1, bd = 0;
	for (n = 0; n < am_nvmode(); n++) {
		am_vminfo(n, &fi);
		if (fi.fi_width == w && fi.fi_height == h && fi.fi_depth <= 8 && fi.fi_depth > bd)
			best = n, bd = fi.fi_depth;
	}
	return best;
}
static int sext(v) int v; { return (v & 64) ? (v & 127) - 128 : v & 127; }
static void mouse(unit, kind, r0, changed) int unit, kind, changed; unsigned long r0;
{
	int x = sext(r0), y = sext(r0 >> 8);
	mx += x; my += y; packets++;
	if (x < -64 || x > 63 || y < -64 || y > 63) bounds++;
}
void *AllocMem(n, flags) unsigned long n; int flags;
{
	void *p;
	allocno++;
	if (allocno == failat) return 0;
	p = malloc(n); if (p) live++;
	return p;
}
void FreeMem(p, n) void *p; unsigned long n; { free(p); live--; }
struct screen *OpenScreen() { if (openfail) return 0; service.flags = Sf_INUSE; return &service; }
void NewCop(p) struct screen *p; { }
void normcop(p, v, b, offset) struct screen *p; struct coplist *v; unsigned short *b; unsigned offset;
{ v->LOFlist = v->SHFlist = b; }
void DisplayScreen(p) struct screen *p; { displayed = p == &native ? 1 : 2; }
void SelectScreen(p) struct screen *p; { selected = p == &native ? 1 : 2; activescreen = p; }
void ds_vblintr() { frames++; }
void ds_am_console() { }
void __amix_kbintr() { }
int splscr() { return 0; }
void splx(v) int v; { }
#define CHECK(x) do { if (!(x)) { printf("FAIL %d\n", __LINE__); return 1; } checks++; } while (0)
int main()
{
	int checks = 0, i, x, y;
	struct dssess s;
	failat = 1; CHECK(am_dsinit() == ENOMEM && live == 0);
	allocno = 0; failat = 2; CHECK(am_dsinit() == ENOMEM && live == 0);
	allocno = 0; failat = 0; openfail = 1; CHECK(am_dsinit() == ENOMEM && live == 0);
	openfail = 0; native.flags = Sf_INUSE;
	CHECK(am_dsinit() == 0 && live == 2);
	CHECK((fbcons.fc_m.fm_base & DS_PGOFF) == 0);
	CHECK(fbcons.fc_m.fm_width == 640 && fbcons.fc_m.fm_height == 200 && fbcons.fc_m.fm_depth == 1);
	CHECK(service.bmap == &am_bitmap && service.bmap->bpl[0] == (unsigned char *)fbcons.fc_m.fm_base);
	CHECK(am_dsinit() == 0 && live == 2);
	CHECK(fbcons_grab() == 1 && fbcons_grab() == 0); fbcons_unlock(); CHECK(fbcons_grab() == 1); fbcons_unlock();
	ds_disp.d_base = fbcons.fc_m.fm_base;
	ds_disp.d_info.fi_width = 640; ds_disp.d_info.fi_height = 200;
	ds_disp.d_info.fi_depth = 1; ds_disp.d_info.fi_rowbytes = 80;
	ds_disp.d_info.fi_size = (16000 + DS_PGOFF) & ~DS_PGOFF;
	am_front(&ds_sess[1]); CHECK(selected == 2 && displayed == 2 && service.depth == 1);
	am_front(&ds_sess[0]); CHECK(selected == 1 && displayed == 1 && native.flags == Sf_INUSE);
	/* planar modes: 1-4 planes, 640x200 and interlaced 640x400 */
	CHECK(am_nvmode() == 8 && am_dsvsize == ((4 * 32000 + DS_PGOFF) & ~DS_PGOFF));
	CHECK(am_vmfind(0x94UL) == 3 && am_vmfind(0x99UL) == 4 && am_vmfind(0x90UL) < 0 && am_vmfind(0x95UL) < 0);
	{
		struct fbinfo fi;
		am_vminfo(3, &fi);
		CHECK(fi.fi_layout == FBL_PLANES && fi.fi_depth == 4 && fi.fi_planebytes == 16000 &&
		    fi.fi_cmapsize == 16 && fi.fi_mode == 0x94 && fi.fi_size >= 64000 && fi.fi_size <= am_dsvsize);
		am_vminfo(7, &fi);
		CHECK(fi.fi_height == 400 && fi.fi_planebytes == 32000 && fi.fi_size == am_dsvsize);
	}
	ds_sess[1].s_vmode = 4; am_front(&ds_sess[1]);
	CHECK(service.depth == 4 && service.height == 200 && !(service.modes & Sm_ILACE) &&
	    am_bitmap.bpl[3] == (unsigned char *)fbcons.fc_m.fm_base + 3 * 16000);
	s.s_blank = 0; s.s_cmap[0][15] = 0xf000; s.s_cmap[1][15] = s.s_cmap[2][15] = 0;
	am_dsclut(&s, 0, 16); CHECK(service.color[15] == 0xf00);
	ds_sess[1].s_vmode = 8; am_front(&ds_sess[1]);
	CHECK(service.depth == 4 && service.height == 400 && (service.modes & Sm_ILACE));
	ds_sess[1].s_vmode = 0; am_front(&ds_sess[1]);
	CHECK(service.depth == 1 && service.height == 200 && !(service.modes & Sm_ILACE));
	am_front(&ds_sess[0]);
	for (i = 0; i < 128; i++) CHECK(am_toraw[i] == 255 || am_toraw[i] < 0x68);
	s.s_blank = 0; s.s_cmap[0][0] = 0xf000; s.s_cmap[1][0] = 0xa000; s.s_cmap[2][0] = 0x5000;
	am_dsclut(&s, 0, 1); CHECK(service.color[0] == 0xfa5);
	s.s_blank = 1; am_dsclut(&s, 0, 1); CHECK(service.color[0] == 0);
	am_misc(&service, SC_VBCALL); CHECK(frames == 1);
	am_mouse = mouse;
	for (x = -128; x < 128; x++)
		for (y = -128; y < 128; y++) {
			mx = my = packets = bounds = 0;
			am_input(&service, 0x02ff0000UL | ((unsigned long)(x & 255) << 8) | (y & 255), 0);
			CHECK(mx == x && my == y && packets <= 3 && bounds == 0);
		}
	/* card modes: the timings up to the attach size, each depth */
	trtg.r_width = 1024; trtg.r_height = 768; trtg.r_row = 1024; trtg.r_depths = 7;
	trtg.r_setdepth = tnop; trtg.r_show = tnop;
	trtg.r_modes = tmodes; trtg.r_nmode = 4; trtg.r_mode = 2; trtg.r_setmode = tset;
	am_rtg = &trtg; am_dsfull = 1; am_tims(); am_curt = 2; am_curbits = 8;
	ds_disp.d_info.fi_width = 1024; ds_disp.d_info.fi_height = 768;
	CHECK(am_nvmode() == 9);
	{
		struct fbinfo fi;
		unsigned long hz;
		am_vminfo(0, &fi);
		CHECK(fi.fi_width == 640 && fi.fi_height == 480 && fi.fi_depth == 8 && fi.fi_rowbytes == 640 &&
		    fi.fi_mode == 0x1a8);
		am_vminfo(5, &fi);
		CHECK(fi.fi_width == 1024 && fi.fi_depth == 16 && fi.fi_rowbytes == 2048 && fi.fi_mode == 0xb0);
		CHECK(am_vmfind(0xa8UL) == 2 && am_vmfind(0x1c0UL) == 6 && am_vmfind(0x4a8UL) < 0);
		CHECK(am_vmflags(2, &hz) == FBM_TRIED && hz == 60 && am_vmflags(3, &hz) == 0);
		/* a pan keeps the whole mode inside two frames, not just a centred screen */
		hz = fi.fi_rowbytes * fi.fi_height;
		CHECK(DS_PANOK(hz, &fi, 2 * hz) && !DS_PANOK(hz + 2048, &fi, 2 * hz) &&
		    !DS_PANOK(8UL, &fi, 2 * hz) && !DS_PANOK(3 * hz, &fi, 2 * hz));
	}
	ds_sess[1].s_vmode = 4; setn = 0; am_planes(&ds_sess[1]);
	CHECK(setn == 1 && sett == 0 && setb == 16);
	am_planes(&ds_sess[1]); CHECK(setn == 1);
	ds_sess[1].s_vmode = 0; am_planes(&ds_sess[1]); CHECK(setn == 2 && sett == 2 && setb == 8);
	am_rtgpanic(); CHECK(setn == 2);
	am_rtg = 0;
	/* the MiSTer board: found by its product, its ID checked, its modes to the attach size */
	{
		struct screen *sv = am_screen;
		struct fbinfo fi;
		int t, k;
		mrmem = calloc(1, MR_MAP);
		am_screen = 0;
		CHECK(mr_attach() == ENXIO);
		MREG(R_IDREG) = MR_ID;
		CHECK(mr_attach() == 0 && am_rtg == &mr_rtg && mr_attach() == EBUSY);
		am_screen = sv;
		CHECK(MREG(R_FORMAT) == F_CLUT && MREG(R_HSIZE) == 1280 && MREG(R_VSIZE) == 1024 &&
		    MREG(R_STRIDE) == 1280 && MREG(R_ADDRHI) == 0x2700 && MREG(R_ADDRLO) == 0 && MREG(R_ENABLE) == 0);
		CHECK(mr_rtg.r_depths == 7 && mr_rtg.r_phys == 0x50000000UL && mr_rtg.r_base == (unsigned long)mrmem);
		mr_clut(255, 0x12, 0x34, 0x56);
		CHECK(MREG(R_CLUT + 1020) == 0x12 && MREG(R_CLUT + 1022) == 0x3456);
		mr_show(1); CHECK(MREG(R_ENABLE) == 1);
		ds_disp.d_info.fi_width = 1280; ds_disp.d_info.fi_height = 1024;
		am_dsfull = 1; am_tims(); am_curt = am_mtiming(-1); am_curbits = 8;
		/* 640x480 to 1280x1024, each at 8, 16 and 32 bits */
		CHECK(am_ntim == 5 && am_nvmode() == 15);
		/* the Mac and TOS sessions' default, the attach mode at 8 bits */
		ds_sess[1].s_vmode = 0; am_planes(&ds_sess[1]);
		CHECK(MREG(R_FORMAT) == F_CLUT && MREG(R_STRIDE) == 1280);
		/* the Amiga, Mac and TOS rules pick 8 bits at each size the card lists */
		for (k = 0; k < am_ntim; k++) {
			t = envpick(mr_modes[am_tim[k]].m_width, mr_modes[am_tim[k]].m_height);
			CHECK(t == k);
			am_vminfo(t, &fi);
			CHECK(fi.fi_depth == 8 && fi.fi_visual != FBV_TRUE && fi.fi_rowbytes == fi.fi_width);
			ds_sess[1].s_vmode = t + 1; am_planes(&ds_sess[1]);
			CHECK(MREG(R_FORMAT) == F_CLUT && MREG(R_HSIZE) == fi.fi_width &&
			    MREG(R_VSIZE) == fi.fi_height && MREG(R_STRIDE) == fi.fi_width);
		}
		ds_sess[1].s_vmode = 2 * am_ntim + 1; am_planes(&ds_sess[1]);
		CHECK(MREG(R_FORMAT) == F_RGB32 && MREG(R_HSIZE) == 640 && MREG(R_STRIDE) == 2560);
		ds_sess[1].s_vmode = am_ntim + 3; am_planes(&ds_sess[1]);
		CHECK(MREG(R_FORMAT) == F_565 && MREG(R_HSIZE) == 1024 && MREG(R_STRIDE) == 2048);
		am_screen = 0; CHECK(mr_detach() == 0 && am_rtg == 0 && MREG(R_ENABLE) == 0);
		/* 1080p: the largest, 32 bits still fit */
		mr_width = 1920; mr_height = 1080;
		CHECK(mr_attach() == 0 && mr_rtg.r_depths == 7);
		am_tims(); CHECK(am_ntim == 6);
		CHECK(mr_detach() == 0);
		mr_width = 1000; CHECK(mr_attach() == EINVAL);
		am_screen = sv;
		am_dsfull = 0;
	}
	printf("PASS %d display adapter checks\n", checks);
	return 0;
}
