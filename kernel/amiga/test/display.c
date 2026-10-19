#include "../amigads.c"
extern void *malloc();
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
	printf("PASS %d display adapter checks\n", checks);
	return 0;
}
