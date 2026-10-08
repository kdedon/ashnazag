/*
 * meta.c -- metafiles: a picture recorded and played back must come out
 * as the same picture drawn directly; the records counted by
 * EnumMetaFile; the bits passed through Get/SetMetaFileBits; a disk
 * metafile written by CloseMetaFile and read back by GetMetaFile; one
 * metafile played into another.  Each result a line of RESULT.TXT
 * against expect/meta.txt.
 */
#include <windows.h>
#include <stdio.h>
#include "result.h"

#define	W	96
#define	H	64

static HBITMAP src;

/* the picture, on any DC */
static void
picture(HDC dc)
{
	HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0)), op;
	HBRUSH br = CreateSolidBrush(RGB(0, 0, 255)), hb = CreateHatchBrush(HS_CROSS, RGB(0, 128, 0)), ob;
	HRGN rgn = CreateEllipticRgn(60, 30, 90, 60);
	HDC mem;
	POINT pts[3];
	int dx[3];

	SetBkColor(dc, RGB(255, 255, 255));
	op = SelectObject(dc, pen);
	ob = SelectObject(dc, br);
	Rectangle(dc, 2, 2, 30, 20);
	SelectObject(dc, hb);
	Ellipse(dc, 34, 2, 60, 22);
	MoveTo(dc, 0, 63);
	LineTo(dc, 95, 30);
	pts[0].x = 64; pts[0].y = 2;
	pts[1].x = 92; pts[1].y = 2;
	pts[2].x = 78; pts[2].y = 24;
	SelectObject(dc, br);
	Polygon(dc, pts, 3);
	SetTextColor(dc, RGB(0, 0, 0));
	SetBkMode(dc, TRANSPARENT);
	TextOut(dc, 2, 24, "Meta", 4);
	dx[0] = 9; dx[1] = 9; dx[2] = 9;
	ExtTextOut(dc, 2, 42, 0, NULL, "abc", 3, dx);
	FillRgn(dc, rgn, hb);
	/* a bitmap's pixels through a memory DC */
	mem = CreateCompatibleDC(dc);
	if (mem) {
		HBITMAP old = SelectObject(mem, src);

		BitBlt(dc, 40, 40, 16, 16, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteDC(mem);
	}
	PatBlt(dc, 30, 30, 6, 6, BLACKNESS);
	SaveDC(dc);
	IntersectClipRect(dc, 0, 0, 20, 64);
	SelectObject(dc, br);
	Rectangle(dc, 10, 50, 40, 62);
	RestoreDC(dc, -1);
	SelectObject(dc, op);
	SelectObject(dc, ob);
	DeleteObject(pen);
	DeleteObject(br);
	DeleteObject(hb);
	DeleteObject(rgn);
}

/* a white bitmap with what draw puts on it; its pixels compared with another's */
static HBITMAP
white(HDC screen)
{
	HBITMAP b = CreateCompatibleBitmap(screen, W, H);
	HDC m = CreateCompatibleDC(screen);
	HBITMAP o = SelectObject(m, b);

	PatBlt(m, 0, 0, W, H, WHITENESS);
	SelectObject(m, o);
	DeleteDC(m);
	return b;
}

static long
differ(HDC screen, HBITMAP a, HBITMAP b)
{
	HDC ma = CreateCompatibleDC(screen), mb = CreateCompatibleDC(screen);
	HBITMAP oa = SelectObject(ma, a), ob = SelectObject(mb, b);
	long n = 0;
	int x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			if (GetPixel(ma, x, y) != GetPixel(mb, x, y))
				n++;
	SelectObject(ma, oa);
	SelectObject(mb, ob);
	DeleteDC(ma);
	DeleteDC(mb);
	return n;
}

static void
playon(HDC screen, HBITMAP b, HMETAFILE mf)
{
	HDC m = CreateCompatibleDC(screen);
	HBITMAP o = SelectObject(m, b);

	PlayMetaFile(m, mf);
	SelectObject(m, o);
	DeleteDC(m);
}

static int records, kinds[16];

int FAR PASCAL
counter(HDC dc, HANDLETABLE FAR *ht, METARECORD FAR *mr, int n, LPARAM data)
{
	records++;
	if (mr->rdFunction == META_TEXTOUT)
		kinds[0]++;
	if (mr->rdFunction == META_SELECTOBJECT)
		kinds[1]++;
	if (mr->rdFunction == META_DIBBITBLT)
		kinds[2]++;
	if (mr->rdFunction == META_CREATEREGION)
		kinds[3]++;
	PlayMetaFileRecord(dc, ht, mr, n);
	return 1;
}

int PASCAL
WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
	HDC screen = GetDC(0), mdc, m;
	HBITMAP direct, played, again, o;
	HMETAFILE mf, mf2, disk;
	HGLOBAL bits;
	char buf[80];
	FARPROC cb;

	/* a source bitmap with a pattern of its own */
	src = CreateCompatibleBitmap(screen, 16, 16);
	m = CreateCompatibleDC(screen);
	o = SelectObject(m, src);
	PatBlt(m, 0, 0, 16, 16, WHITENESS);
	PatBlt(m, 4, 4, 8, 8, BLACKNESS);
	SelectObject(m, o);
	DeleteDC(m);

	direct = white(screen);
	m = CreateCompatibleDC(screen);
	o = SelectObject(m, direct);
	picture(m);
	SelectObject(m, o);
	DeleteDC(m);

	mdc = CreateMetaFile(NULL);
	sprintf(buf, "metafile dc %d", mdc != 0);
	report(buf);
	picture(mdc);
	mf = CloseMetaFile(mdc);
	sprintf(buf, "closed %d", mf != 0);
	report(buf);

	played = white(screen);
	playon(screen, played, mf);
	sprintf(buf, "played differs %ld", differ(screen, direct, played));
	report(buf);

	/* the records one by one */
	again = white(screen);
	m = CreateCompatibleDC(screen);
	o = SelectObject(m, again);
	cb = MakeProcInstance((FARPROC)counter, hi);
	EnumMetaFile(m, mf, (MFENUMPROC)cb, 0);
	SelectObject(m, o);
	DeleteDC(m);
	sprintf(buf, "enum differs %ld", differ(screen, direct, again));
	report(buf);
	sprintf(buf, "records %d textout %d select %d dib %d region %d", records > 20, kinds[0], kinds[1] >= 6,
	    kinds[2], kinds[3]);
	report(buf);

	/* the bits and back */
	bits = GetMetaFileBits(mf);
	mf = SetMetaFileBits(bits);
	sprintf(buf, "bits %d", mf != 0);
	report(buf);

	/* one played into another */
	mdc = CreateMetaFile(NULL);
	PlayMetaFile(mdc, mf);
	mf2 = CloseMetaFile(mdc);
	DeleteObject(again);
	again = white(screen);
	playon(screen, again, mf2);
	sprintf(buf, "copy differs %ld", differ(screen, direct, again));
	report(buf);

	/* on disk */
	mdc = CreateMetaFile("PIC.WMF");
	picture(mdc);
	DeleteMetaFile(CloseMetaFile(mdc));
	disk = GetMetaFile("PIC.WMF");
	DeleteObject(again);
	again = white(screen);
	if (disk)
		playon(screen, again, disk);
	sprintf(buf, "disk differs %ld", disk ? differ(screen, direct, again) : -1L);
	report(buf);

	DeleteMetaFile(mf);
	DeleteMetaFile(mf2);
	if (disk)
		DeleteMetaFile(disk);
	ReleaseDC(0, screen);
	return 0;
}
