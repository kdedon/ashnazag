/*
 * drv.c -- installable drivers, USER's part of them: OpenDriver,
 * CloseDriver, SendDriverMessage and the rest.  A driver is a DLL with a
 * DriverProc; it is named by a key of SYSTEM.INI's [drivers] (or the
 * section given), whose value is its file and arguments, or by its file.
 * The first open of a module sends DRV_LOAD, DRV_ENABLE; each open
 * DRV_OPEN, whose result is the instance's identifier; the last close
 * DRV_DISABLE, DRV_FREE.  At the start of a session the drivers of
 * [boot] drivers= are opened, as USER does: MMSYSTEM is one, and opens
 * the wave and timer drivers in turn.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "win.h"

#define	DRV_LOAD	1
#define	DRV_ENABLE	2
#define	DRV_OPEN	3
#define	DRV_CLOSE	4
#define	DRV_DISABLE	5
#define	DRV_FREE	6
#define	DRV_CONFIGURE	7
#define	DRV_QUERYCONFIGURE 8
#define	DRV_INSTALL	9
#define	DRV_REMOVE	10

#define	NDRV	32
#define	HBASE	0x5000		/* handles: not 0, not a selector a program would take for one */

struct drv {
	int	used;
	struct module *m;
	u32	proc;		/* its DriverProc */
	u32	id;		/* DRV_OPEN's answer */
	char	alias[128];
	int	order;		/* opening order, for GetNextDriver */
};

static struct drv drvs[NDRV];
static int norder;

static struct drv *
dget(h)
	u32 h;
{
	h &= 0xffff;
	if (h < HBASE || h >= HBASE + NDRV || !drvs[h - HBASE].used)
		return 0;
	return &drvs[h - HBASE];
}

static u32
call(d, msg, lp1, lp2)
	struct drv *d;
	int msg;
	u32 lp1, lp2;
{
	cb_begin();
	cb_push32(d->id);
	cb_push16(HBASE + (d - drvs));
	cb_push16(msg);
	cb_push32(lp1);
	cb_push32(lp2);
	return cb_call(d->proc, 0);
}

/* another open instance of this module */
static int
others(d)
	struct drv *d;
{
	int i;

	for (i = 0; i < NDRV; i++)
		if (drvs[i].used && &drvs[i] != d && drvs[i].m == d->m)
			return 1;
	return 0;
}

/* OpenDriver(name, section, lParam): the handle, 0 if it cannot */
u16
drv_open(name, section, lp)
	char *name, *section;
	u32 lp;
{
	char val[260], file[260], *args;
	struct drv *d;
	struct module *m;
	int i, err, first;
	u32 a;

	if (!name || !*name)
		return 0;
	if (profile_get("SYSTEM.INI", section && *section ? section : "drivers", name, "", val, sizeof val) <= 0)
		strncpy(val, name, sizeof val - 1), val[sizeof val - 1] = 0;
	for (args = val; *args && *args != ' ' && *args != '\t'; args++)
		;
	memcpy(file, val, args - val);
	file[args - val] = 0;
	while (*args == ' ' || *args == '\t')
		args++;
	for (i = 0; i < NDRV && drvs[i].used; i++)
		;
	if (i == NDRV || (m = mod_load(file, &err)) == 0)
		return 0;
	{
		extern void initdeps();

		initdeps(m, 0);
	}
	d = &drvs[i];
	memset((char *)d, 0, sizeof *d);
	d->used = 1;
	d->m = m;
	d->proc = mod_proc(m, 0, "DriverProc");
	strncpy(d->alias, name, sizeof d->alias - 1);
	d->order = ++norder;
	if (!d->proc) {
		mod_free(m);
		d->used = 0;
		return 0;
	}
	first = !others(d);
	if (first && (!(call(d, DRV_LOAD, 0, 0) & 0xffff) || !(call(d, DRV_ENABLE, 0, 0) & 0xffff))) {
		mod_free(m);
		d->used = 0;
		return 0;
	}
	a = ustr(args);
	d->id = call(d, DRV_OPEN, a, lp);
	ufree(a);
	if (!d->id) {
		if (first) {
			call(d, DRV_DISABLE, 0, 0);
			call(d, DRV_FREE, 0, 0);
		}
		mod_free(m);
		d->used = 0;
		return 0;
	}
	if (w16_debug)
		w16_log("startwin: driver %s (%s) open\n", name, file);
	return HBASE + i;
}

static u32
u_OpenDriver(a)
	u32 *a;
{
	return drv_open(gptr(a[0]), gptr(a[1]), a[2]);
}

static u32
u_CloseDriver(a)
	u32 *a;
{
	struct drv *d = dget(a[0]);
	u32 r;

	if (!d)
		return 0;
	r = call(d, DRV_CLOSE, a[1], a[2]);
	if (!others(d)) {
		call(d, DRV_DISABLE, 0, 0);
		call(d, DRV_FREE, 0, 0);
	}
	mod_free(d->m);
	d->used = 0;
	return r;
}

static u32
u_SendDriverMessage(a)
	u32 *a;
{
	struct drv *d = dget(a[0]);

	return d ? call(d, a[1] & 0xffff, a[2], a[3]) : 0;
}

static u32
u_GetDriverModuleHandle(a)
	u32 *a;
{
	struct drv *d = dget(a[0]);

	return d ? d->m->m_hmod : 0;
}

static u32
u_DefDriverProc(a)
	u32 *a;
{
	switch (a[2] & 0xffff) {
	case DRV_LOAD:
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
		return 1;
	case DRV_INSTALL:
	case DRV_REMOVE:
		return 1;	/* DRV_OK */
	}
	return 0;
}

/* GetDriverInfo(h, DRIVERINFOSTRUCT far *): length, hDriver, hModule, szAliasName[128] */
static u32
u_GetDriverInfo(a)
	u32 *a;
{
	struct drv *d = dget(a[0]);
	u32 p = lin(FPSEL(a[1]), FPOFF(a[1]));

	if (!d || !p || GW(p) < 6)
		return 0;
	PW(p + 2, a[0]);
	PW(p + 4, d->m->m_hmod);
	if (GW(p) >= 6 + 128) {
		memset(M + p + 6, 0, 128);
		strncpy((char *)M + p + 6, d->alias, 127);
	}
	return 1;
}

/* GetNextDriver(h, flags): in opening order (GND_REVERSE 2 backwards), GND_FIRSTINSTANCEONLY 1 */
static u32
u_GetNextDriver(a)
	u32 *a;
{
	struct drv *d = dget(a[0]), *best = 0;
	int i, j, rev = (a[1] & 2) != 0, from = d ? d->order : rev ? 1 << 30 : 0, dup;

	for (i = 0; i < NDRV; i++) {
		if (!drvs[i].used || (rev ? drvs[i].order >= from : drvs[i].order <= from))
			continue;
		if (a[1] & 1) {
			for (dup = 0, j = 0; j < NDRV; j++)
				if (drvs[j].used && drvs[j].m == drvs[i].m && drvs[j].order < drvs[i].order)
					dup = 1;
			if (dup)
				continue;
		}
		if (!best || (rev ? drvs[i].order > best->order : drvs[i].order < best->order))
			best = &drvs[i];
	}
	return best ? HBASE + (best - drvs) : 0;
}

/* at the start of the session: SYSTEM.INI [boot] drivers=, each opened */
void
drv_boot()
{
	char list[512], *p, *e;

	if (profile_get("SYSTEM.INI", "boot", "drivers", "", list, sizeof list) <= 0)
		return;
	for (p = list; *p; p = e) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		for (e = p; *e && *e != ' ' && *e != '\t' && *e != ','; e++)
			;
		if (e == p)
			break;
		if (*e)
			*e++ = 0;
		drv_open(p, "drivers", (u32)0);
	}
}

struct impl dv_impl[] = {
	{ "USER", "OpenDriver", u_OpenDriver },
	{ "USER", "CloseDriver", u_CloseDriver },
	{ "USER", "SendDriverMessage", u_SendDriverMessage },
	{ "USER", "GetDriverModuleHandle", u_GetDriverModuleHandle },
	{ "USER", "DefDriverProc", u_DefDriverProc },
	{ "USER", "GetDriverInfo", u_GetDriverInfo },
	{ "USER", "GetNextDriver", u_GetNextDriver },
	{ 0 }
};
