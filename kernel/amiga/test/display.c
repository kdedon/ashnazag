#include "../amigads.c"
extern void *malloc();
extern void free();
extern int printf();
unsigned int displaytype;
struct dssess ds_sess[1 + DS_NSESS];
struct dsdisp ds_disp;
void (*ds_frontfn)();
static struct screen native, service;
struct screen *activescreen = &native;
static int allocno, failat, live, openfail, selected, displayed, frames;
static int mx, my, packets, bounds;
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
	/* a chipset session: the provider shows it, the service stays out */
	CHECK(am_passattach(mouse) == 0 && am_passattach(mouse) == EBUSY);
	ds_sess[1].s_used = ds_sess[1].s_chip = 1; displayed = 0;
	am_front(&ds_sess[1]); CHECK(displayed == 0);
	CHECK(am_passdetach(mouse) == EBUSY);
	ds_sess[1].s_used = ds_sess[1].s_chip = 0;
	CHECK(am_passdetach(mouse) == 0 && am_passfn == 0);
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
	printf("PASS %d display adapter checks\n", checks);
	return 0;
}
