#include "../amigads.c"
extern void *malloc();
extern void free();
extern int printf();
unsigned int displaytype;
struct dssess ds_sess[1 + DS_NSESS];
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
	am_front(&ds_sess[1]); CHECK(selected == 2 && displayed == 2);
	am_front(&ds_sess[0]); CHECK(selected == 1 && displayed == 1 && native.flags == Sf_INUSE);
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
