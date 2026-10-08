/*
 * ne.c -- the NE loader: Win16 executables and DLLs, and our own system
 * modules (apitab) standing where Windows' would.
 *
 * Every segment is loaded at once and stays (nothing is discarded),
 * relocations are applied, and exported prologues are patched as
 * Windows does: `push ds; pop ax; nop' (or `mov ax,ds; nop') becomes
 * `mov ax,DGROUP' in a DLL and three nops in a program, whose DS then
 * comes from the caller (MakeProcInstance thunks, and our callbacks,
 * set AX).  The module database is the NE header in a global block, its
 * selector the module handle.  Resources stay in the file until loaded.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "w16.h"
#include "apitab.h"

struct module *modules;

/* the system modules: ours whatever the disk holds */
static char *ours[] = { "KERNEL", "USER", "GDI", "KEYBOARD", "SYSTEM", "SOUND", "WIN87EM",
	"DISPLAY", "MOUSE", "TOOLHELP", 0 };

#define	NEW(p)	((p)[0] | (p)[1] << 8)
#define	NEL(p)	(NEW(p) | (u32)NEW((p) + 2) << 16)

struct module *
mod_find(name)
	char *name;
{
	struct module *m;

	for (m = modules; m; m = m->m_next)
		if (w16_stricmp(m->m_name, name) == 0)
			return m;
	return 0;
}

struct module *
mod_byhandle(h)
	u32 h;
{
	struct module *m;
	struct desc *d;
	int i;

	h &= 0xffff;
	if (h == 0)
		return 0;
	for (m = modules; m; m = m->m_next)
		if (m->m_hmod == h || m->m_hinst == h)
			return m;
	/* any selector of the module's segments */
	for (m = modules; m; m = m->m_next)
		for (i = 1; i <= m->m_nseg; i++)
			if (m->m_seg[i].ns_sel == (h | 7) || m->m_seg[i].ns_sel == h)
				return m;
	if ((h & 4) && SELIX(h) < LDTSIZE) {
		d = &LDT[SELIX(h)];
		if (d->d_owner)
			for (m = modules; m; m = m->m_next)
				if (m->m_hinst == d->d_owner || m->m_hmod == d->d_owner)
					return m;
	}
	return 0;
}

/* ---- our own modules ---- */

static struct module *
native(name)
	char *name;
{
	struct apimod *am;
	struct apient *e;
	struct module *m;
	int i, n;

	for (i = 0; ours[i]; i++)
		if (w16_stricmp(ours[i], name) == 0)
			break;
	for (am = apimods; am->am_name; am++)
		if (w16_stricmp(am->am_name, name) == 0)
			break;
	if (!am->am_name)
		return 0;
	m = (struct module *)calloc(1, sizeof *m);
	strcpy(m->m_name, am->am_name);
	sprintf(m->m_path, "%s\\%s.%s", sysdir, am->am_name,
	    strcmp(am->am_name, "KERNEL") == 0 || strcmp(am->am_name, "USER") == 0 ||
	    strcmp(am->am_name, "GDI") == 0 ? "EXE" : "DLL");
	m->m_native = 1;
	m->m_dll = 1;
	m->m_api = am;
	m->m_ref = 1;
	for (n = 0, e = am->am_ent; e->ae_name; e++)
		if (e->ae_ord > n)
			n = e->ae_ord;
	m->m_nent = n;
	/* a small module database: the signature and the name */
	m->m_hmod = g_alloc(GMEM_ZEROINIT, 64, 0);
	M[sel_base(m->m_hmod)] = 'N';
	M[sel_base(m->m_hmod) + 1] = 'E';
	PW(sel_base(m->m_hmod) + 2, 1);
	strcpy((char *)M + sel_base(m->m_hmod) + 0x30, m->m_name);
	m->m_hinst = m->m_hmod;
	m->m_next = modules;
	modules = m;
	return m;
}

/* ---- finding files ---- */

/* the module file for name: 0 found (path set), else -1 */
static int
findmod(name, from, dos, host)
	char *name, *dos, *host;
	struct module *from;
{
	static char *exts[] = { "", ".DLL", ".EXE", ".DRV", ".FON", 0 };
	char dirs[4][260], try[300], *p;
	int i, j, nd = 0;
	FILE *fp;

	if (strchr(name, '\\') || strchr(name, ':') || strchr(name, '/')) {
		strcpy(dirs[nd++], "");
	} else {
		if (from && from->m_path[0]) {
			strcpy(dirs[nd], from->m_path);
			if ((p = strrchr(dirs[nd], '\\')) != 0) {
				*p = 0;
				nd++;
			}
		}
		strcpy(dirs[nd++], ".");
		strcpy(dirs[nd++], windir);
		strcpy(dirs[nd++], sysdir);
	}
	for (i = 0; i < nd; i++)
		for (j = 0; exts[j]; j++) {
			if (*exts[j] && strchr(name, '.'))
				continue;
			if (!*exts[j] && !strchr(name, '.') && nd > 1)
				continue;
			if (dirs[i][0] == 0)
				sprintf(try, "%s%s", name, exts[j]);
			else if (strcmp(dirs[i], ".") == 0)
				sprintf(try, "%s%s", name, exts[j]);
			else
				sprintf(try, "%s\\%s%s", dirs[i], name, exts[j]);
			if (dos_fullpath(try, dos) != 0)
				continue;
			if (dos_hostpath(dos, host, 1024, 0) != 0)
				continue;
			if ((fp = fopen(host, "rb")) != 0) {
				fclose(fp);
				return 0;
			}
		}
	return -1;
}

/* ---- loading ---- */

static int
rdat(fp, off, buf, n)
	FILE *fp;
	u32 off, n;
	u8 *buf;
{
	if (fseek(fp, off, 0) != 0)
		return -1;
	return fread(buf, 1, n, fp) == n ? 0 : -1;
}

/* the far pointer an import or internal reference names */
static u32
target(m, ne, r, err)
	struct module *m;
	u8 *ne, *r;
	int *err;
{
	int flags = r[1] & 3, mi, ord;
	u32 p;
	struct module *t;
	char name[64];
	u8 *in;

	switch (flags) {
	case 0:		/* internal */
		if (r[4] == 0xff) {
			ord = NEW(r + 6);
			return mod_proc(m, ord, (char *)0);
		}
		if (r[4] == 0 || r[4] > m->m_nseg)
			return 0;
		return FP(m->m_seg[r[4]].ns_sel, NEW(r + 6));
	case 1:		/* import by ordinal */
	case 2:		/* import by name */
		mi = NEW(r + 4);
		if (mi < 1 || mi > m->m_nimp || !(t = m->m_imp[mi - 1]))
			return 0;
		if (flags == 1) {
			ord = NEW(r + 6);
			p = mod_proc(t, ord, (char *)0);
			if (!p) {
				w16_log("%s: %s.%d is not there\n", m->m_name, t->m_name, ord);
				*err = 1;
			}
			return p;
		}
		in = ne + NEW(ne + 0x2a) + NEW(r + 6);
		memcpy(name, in + 1, in[0]);
		name[in[0]] = 0;
		p = mod_proc(t, 0, name);
		if (!p) {
			w16_log("%s: %s.%s is not there\n", m->m_name, t->m_name, name);
			*err = 1;
		}
		return p;
	}
	return 0;		/* OS fixups: the floating-point emulator's; see the INT 34h handler */
}

static void
relocate(m, ne, segn, recs, n)
	struct module *m;
	u8 *ne, *recs;
	int segn, n;
{
	u32 base = sel_base(m->m_seg[segn].ns_sel), lim = m->m_seg[segn].ns_alloc;
	u32 p, off, next, v, w;
	int i, type, additive, err = 0, guard;
	u8 *r;

	for (i = 0; i < n; i++) {
		r = recs + 8 * i;
		type = r[0];
		additive = r[1] & 4;
		if ((r[1] & 3) == 3)
			continue;
		p = target(m, ne, r, &err);
		off = NEW(r + 2);
		w = type == 0 ? 1 : type == 3 ? 4 : 2;
		for (guard = 0; guard < 0x10000; guard++) {
			if (off + w > lim)
				break;
			next = w == 1 ? 0xffff : GW(base + off);
			switch (type) {
			case 0:		/* low byte */
				v = additive ? GB(base + off) + p : p;
				PB(base + off, v);
				break;
			case 2:		/* selector */
				PW(base + off, FPSEL(p));
				break;
			case 3:		/* far pointer */
				if (additive)
					PW(base + off, GW(base + off) + FPOFF(p));
				else
					PW(base + off, FPOFF(p));
				PW(base + off + 2, FPSEL(p));
				break;
			case 5:		/* offset */
				if (additive)
					PW(base + off, GW(base + off) + FPOFF(p));
				else
					PW(base + off, FPOFF(p));
				break;
			default:
				w16_log("%s: relocation type %d not done\n", m->m_name, type);
				additive = 1;
				break;
			}
			if (additive || next == 0xffff)
				break;
			off = next;
		}
	}
}

/* exported prologues: DLL mov ax,DGROUP; program nops */
static void
patchentry(m, seg, off)
	struct module *m;
	int seg;
	u32 off;
{
	u32 a;
	u16 dg;

	if (seg < 1 || seg > m->m_nseg || (m->m_seg[seg].ns_flags & NSF_DATA))
		return;
	if (off + 3 > m->m_seg[seg].ns_alloc)
		return;
	a = sel_base(m->m_seg[seg].ns_sel) + off;
	if (!((M[a] == 0x1e && M[a + 1] == 0x58 && M[a + 2] == 0x90) ||
	    (M[a] == 0x8c && M[a + 1] == 0xd8 && M[a + 2] == 0x90)))
		return;
	if (m->m_dll) {
		dg = m->m_dgroup ? m->m_seg[m->m_dgroup].ns_sel : 0;
		if (!dg)
			return;
		M[a] = 0xb8;
		M[a + 1] = dg;
		M[a + 2] = dg >> 8;
	} else
		M[a] = M[a + 1] = M[a + 2] = 0x90;
}

static int
entries(m, ne, len)
	struct module *m;
	u8 *ne;
	u32 len;
{
	u8 *p = ne + NEW(ne + 4), *end = p + NEW(ne + 6);
	int ord = 1, cnt, type, i, n = 0;

	/* count first */
	while (p < end && p[0]) {
		cnt = p[0];
		type = p[1];
		n += cnt;
		p += 2;
		if (type == 0)
			continue;
		p += cnt * (type == 0xff ? 6 : 3);
	}
	m->m_nent = n;
	m->m_ent = (struct nentry *)calloc(n + 1, sizeof *m->m_ent);
	p = ne + NEW(ne + 4);
	while (p < end && p[0]) {
		cnt = p[0];
		type = p[1];
		p += 2;
		if (type == 0) {
			ord += cnt;
			continue;
		}
		for (i = 0; i < cnt; i++, ord++) {
			if (type == 0xff) {
				m->m_ent[ord].ne_flags = p[0];
				m->m_ent[ord].ne_seg = p[3];
				m->m_ent[ord].ne_off = NEW(p + 4);
				p += 6;
			} else {
				m->m_ent[ord].ne_flags = p[0];
				m->m_ent[ord].ne_seg = type;
				m->m_ent[ord].ne_off = NEW(p + 1);
				p += 3;
			}
		}
	}
	return 0;
}

/* the name a resident or nonresident table gives an ordinal */
char *
ne_resname(m, ord)
	struct module *m;
	int ord;
{
	static char name[80];
	u8 *p, *end;
	int pass;

	if (m->m_native) {
		struct apient *e;

		for (e = m->m_api->am_ent; e->ae_name; e++)
			if (e->ae_ord == ord)
				return e->ae_name;
		return "?";
	}
	for (pass = 0; pass < 2; pass++) {
		if (pass == 0) {
			p = m->m_ne + NEW(m->m_ne + 0x26);
			end = m->m_ne + NEW(m->m_ne + 0x28);
		} else {
			if (!m->m_nonres)
				break;
			p = m->m_nonres;
			end = p + m->m_nonreslen;
		}
		p += p[0] + 3;		/* the module name or description */
		while (p < end && p[0]) {
			if (NEW(p + 1 + p[0]) == ord) {
				memcpy(name, p + 1, p[0]);
				name[p[0]] = 0;
				return name;
			}
			p += p[0] + 3;
		}
	}
	return "?";
}

int
mod_ordinal(m, name)
	struct module *m;
	char *name;
{
	u8 *p, *end;
	int pass, n = strlen(name);

	if (m->m_native) {
		struct apient *e;

		for (e = m->m_api->am_ent; e->ae_name; e++)
			if (w16_stricmp(e->ae_name, name) == 0)
				return e->ae_ord;
		return 0;
	}
	for (pass = 0; pass < 2; pass++) {
		if (pass == 0) {
			p = m->m_ne + NEW(m->m_ne + 0x26);
			end = m->m_ne + NEW(m->m_ne + 0x28);
		} else {
			if (!m->m_nonres)
				break;
			p = m->m_nonres;
			end = p + m->m_nonreslen;
		}
		p += p[0] + 3;
		while (p < end && p[0]) {
			if (p[0] == n && w16_strnicmp((char *)p + 1, name, n) == 0)
				return NEW(p + 1 + p[0]);
			p += p[0] + 3;
		}
	}
	return 0;
}

u32
mod_proc(m, ord, name)
	struct module *m;
	int ord;
	char *name;
{
	struct nentry *e;

	if (name) {
		if (name[0] == '#')
			ord = atoi(name + 1);
		else if ((ord = mod_ordinal(m, name)) == 0)
			return 0;
	}
	if (m->m_native)
		return thunk_native(m, ord);
	if (ord < 1 || ord > m->m_nent)
		return 0;
	e = &m->m_ent[ord];
	if (e->ne_seg == 0)
		return 0;
	if (e->ne_seg == 0xfe)
		return FP(e->ne_off, e->ne_off);
	if (e->ne_seg > m->m_nseg)
		return 0;
	return FP(m->m_seg[e->ne_seg].ns_sel, e->ne_off);
}

static struct module *loadfile();

struct module *
mod_load(name, errp)
	char *name;
	int *errp;
{
	return loadfile(name, (struct module *)0, errp);
}

static struct module *
loadfile(name, from, errp)
	char *name;
	struct module *from;
	int *errp;
{
	char base[16], dos[260], host[1024], *p, *q;
	u8 hdr[64], *ne, *seg, *recs;
	struct module *m;
	FILE *fp;
	u32 neoff, nelen, off, len, alloc, sz;
	int i, n, err, isours = 0;

	*errp = 0;
	/* the module name: the file's base name */
	p = name;
	if ((q = strrchr(p, '\\')) != 0)
		p = q + 1;
	if ((q = strrchr(p, '/')) != 0)
		p = q + 1;
	if ((q = strrchr(p, ':')) != 0)
		p = q + 1;
	for (i = 0; *p && *p != '.' && i < 8; p++, i++)
		base[i] = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
	base[i] = 0;
	if ((m = mod_find(base)) != 0) {
		m->m_ref++;
		return m;
	}
	for (i = 0; ours[i]; i++)
		if (strcmp(ours[i], base) == 0)
			isours = 1;
	if (isours)
		return native(base);
	if (findmod(name, from, dos, host) != 0) {
		/* a system module of Windows we have a table for, never seen on disk */
		if ((m = native(base)) != 0)
			return m;
		*errp = 2;		/* file not found */
		return 0;
	}
	if ((fp = fopen(host, "rb")) == 0) {
		*errp = 2;
		return 0;
	}
	if (rdat(fp, 0, hdr, 64) || hdr[0] != 'M' || hdr[1] != 'Z') {
		fclose(fp);
		*errp = 11;		/* bad format */
		return 0;
	}
	neoff = NEL(hdr + 0x3c);
	if (rdat(fp, neoff, hdr, 64) || hdr[0] != 'N' || hdr[1] != 'E') {
		fclose(fp);
		*errp = 11;
		return 0;
	}
	if (NEW(hdr + 0x0c) & 0x0800) {
		w16_log("%s: a self-loading program; not supported\n", name);
		fclose(fp);
		*errp = 11;
		return 0;
	}
	/* the header and its tables, up to the first segment's data or the nonresident names */
	nelen = NEL(hdr + 0x2c) ? NEL(hdr + 0x2c) - neoff : 0x4000;
	if (nelen > 0x10000 || nelen < 64)
		nelen = 0x10000;
	ne = (u8 *)calloc(1, nelen);
	fseek(fp, neoff, 0);
	nelen = fread(ne, 1, nelen, fp);
	m = (struct module *)calloc(1, sizeof *m);
	m->m_ne = ne;
	m->m_nelen = nelen;
	m->m_neoff = neoff;
	strcpy(m->m_path, dos);
	strcpy(m->m_host, host);
	m->m_ref = 1;
	m->m_flags = NEW(ne + 0x0c);
	m->m_dll = (m->m_flags & 0x8000) != 0;
	m->m_dgroup = NEW(ne + 0x0e);
	m->m_heap = NEW(ne + 0x10);
	m->m_stack = NEW(ne + 0x12);
	m->m_ip = NEW(ne + 0x14);
	m->m_cs = NEW(ne + 0x16);
	m->m_sp = NEW(ne + 0x18);
	m->m_ss = NEW(ne + 0x1a);
	m->m_nseg = NEW(ne + 0x1c);
	if (m->m_nseg > MAXSEG) {
		free(ne);
		free(m);
		fclose(fp);
		*errp = 11;
		return 0;
	}
	/* the module name from the resident table */
	p = (char *)ne + NEW(ne + 0x26);
	n = (u8)p[0] > 15 ? 15 : (u8)p[0];
	memcpy(m->m_name, p + 1, n);
	m->m_name[n] = 0;
	w16_upper(m->m_name);
	if (!m->m_name[0])
		strcpy(m->m_name, base);
	/* nonresident names */
	if ((m->m_nonreslen = NEW(ne + 0x20)) != 0) {
		m->m_nonres = (u8 *)malloc(m->m_nonreslen);
		if (rdat(fp, NEL(ne + 0x2c), m->m_nonres, m->m_nonreslen)) {
			free(m->m_nonres);
			m->m_nonres = 0;
			m->m_nonreslen = 0;
		}
	}
	/* the module database */
	m->m_hmod = g_alloc(GMEM_ZEROINIT, nelen, 0);
	memcpy(M + sel_base(m->m_hmod), ne, nelen);
	PW(sel_base(m->m_hmod) + 2, 1);
	m->m_next = modules;
	modules = m;
	/* segments */
	for (i = 1; i <= m->m_nseg; i++) {
		seg = ne + NEW(ne + 0x22) + 8 * (i - 1);
		off = (u32)NEW(seg) << NEW(ne + 0x32);
		len = NEW(seg + 2);
		alloc = NEW(seg + 6);
		if (len == 0 && off)
			len = 0x10000;
		if (alloc == 0)
			alloc = 0x10000;
		if (alloc < len)
			alloc = len;
		m->m_seg[i].ns_flags = NEW(seg + 4);
		m->m_seg[i].ns_size = off ? len : 0;
		if (i == m->m_dgroup) {
			sz = alloc + m->m_stack + m->m_heap;
			if (m->m_ss == i && m->m_sp == 0 && !m->m_dll)
				;
			alloc = sz > 0x10000 ? 0x10000 : sz;
		}
		m->m_seg[i].ns_alloc = alloc;
		m->m_seg[i].ns_sel = g_alloc(GMEM_ZEROINIT, alloc, 0);
		if (!m->m_seg[i].ns_sel)
			w16_fatal("%s: no memory for segment %d", m->m_name, i);
		if (off && rdat(fp, off, M + sel_base(m->m_seg[i].ns_sel), m->m_seg[i].ns_size))
			w16_log("%s: segment %d short\n", m->m_name, i);
	}
	if (m->m_dgroup)
		m->m_hinst = m->m_seg[m->m_dgroup].ns_sel;
	else
		m->m_hinst = m->m_hmod;
	for (i = 1; i <= m->m_nseg; i++) {
		struct gblock *b = g_block(m->m_seg[i].ns_sel);

		b->gb_owner = m->m_hmod;
		LDT[SELIX(m->m_seg[i].ns_sel)].d_owner = m->m_hmod;
		if (!(m->m_seg[i].ns_flags & NSF_DATA)) {
			b->gb_code = 1;
			LDT[SELIX(m->m_seg[i].ns_sel)].d_acc = D_P | D_S | D_CODE | D_R | D_A;
		}
	}
	g_block(m->m_hmod)->gb_owner = m->m_hmod;
	entries(m, ne, nelen);
	/* imports */
	m->m_nimp = NEW(ne + 0x1e);
	if (m->m_nimp > 64)
		m->m_nimp = 64;
	for (i = 0; i < m->m_nimp; i++) {
		char iname[16];
		u8 *in = ne + NEW(ne + 0x2a) + NEW(ne + NEW(ne + 0x28) + 2 * i);

		n = in[0] > 15 ? 15 : in[0];
		memcpy(iname, in + 1, n);
		iname[n] = 0;
		m->m_imp[i] = loadfile(iname, m, &err);
		if (!m->m_imp[i])
			w16_log("%s: needs %s, which is not there\n", m->m_name, iname);
	}
	/* relocations */
	for (i = 1; i <= m->m_nseg; i++) {
		if (!(m->m_seg[i].ns_flags & NSF_RELOC) || !m->m_seg[i].ns_size)
			continue;
		seg = ne + NEW(ne + 0x22) + 8 * (i - 1);
		off = ((u32)NEW(seg) << NEW(ne + 0x32)) + m->m_seg[i].ns_size;
		if (rdat(fp, off, hdr, 2))
			continue;
		n = NEW(hdr);
		recs = (u8 *)malloc(8 * n + 8);
		if (rdat(fp, off + 2, recs, 8 * n) == 0)
			relocate(m, ne, i, recs, n);
		free(recs);
	}
	fclose(fp);
	for (i = 1; i <= m->m_nent; i++)
		if ((m->m_ent[i].ne_flags & 1) && m->m_ent[i].ne_seg && m->m_ent[i].ne_seg < 0xfe)
			patchentry(m, m->m_ent[i].ne_seg, m->m_ent[i].ne_off);
	return m;
}

void
mod_free(m)
	struct module *m;
{
	struct module **pp;
	int i;

	if (!m || --m->m_ref > 0)
		return;
	if (m->m_native) {
		m->m_ref = 1;	/* ours stay */
		return;
	}
	for (pp = &modules; *pp; pp = &(*pp)->m_next)
		if (*pp == m) {
			*pp = m->m_next;
			break;
		}
	for (i = 0; i < m->m_nimp; i++)
		if (m->m_imp[i])
			mod_free(m->m_imp[i]);
	g_freeowner(m->m_hmod);
	g_free(m->m_hmod);
	free(m->m_ne);
	free(m->m_ent);
	if (m->m_nonres)
		free(m->m_nonres);
	free(m);
}

/* ---- resources ---- */

/* a resource name or type (far pointer: integer if the selector is 0) against a table name */
static int
resmatch(m, tab, id, want)
	struct module *m;
	u8 *tab;
	u32 id, want;
{
	char *s, buf[80];
	u8 *n;

	if (FPSEL(want) == 0)
		return (id & 0x8000) && (id & 0x7fff) == FPOFF(want);
	s = gptr(want);
	if (!s)
		return 0;
	if (s[0] == '#')
		return (id & 0x8000) && (id & 0x7fff) == (u32)atoi(s + 1);
	if (id & 0x8000)
		return 0;
	n = tab + id;
	if (n >= m->m_ne + m->m_nelen)
		return 0;
	memcpy(buf, n + 1, n[0]);
	buf[n[0]] = 0;
	return w16_stricmp(buf, s) == 0;
}

/* handle: the entry's offset in the module database */
u32
res_find(m, type, name)
	struct module *m;
	u32 type, name;
{
	u8 *tab, *p;
	int cnt, i;

	if (!m || m->m_native || NEW(m->m_ne + 0x24) == NEW(m->m_ne + 0x26))
		return 0;
	tab = m->m_ne + NEW(m->m_ne + 0x24);
	p = tab + 2;
	while (p + 8 <= m->m_ne + m->m_nelen && NEW(p)) {
		cnt = NEW(p + 2);
		if (resmatch(m, tab, NEW(p), type)) {
			for (i = 0; i < cnt; i++)
				if (resmatch(m, tab, NEW(p + 8 + 12 * i + 6), name))
					return p + 8 + 12 * i - m->m_ne;
			return 0;
		}
		p += 8 + 12 * cnt;
	}
	return 0;
}

u32
res_size(m, h)
	struct module *m;
	u32 h;
{
	int shift;

	if (!m || !h || h + 12 > m->m_nelen)
		return 0;
	shift = NEW(m->m_ne + NEW(m->m_ne + 0x24));
	return (u32)NEW(m->m_ne + h + 2) << shift;
}

u16
res_load(m, h)
	struct module *m;
	u32 h;
{
	u32 off, len;
	u16 g;
	int shift;
	FILE *fp;

	if (!m || !h || h + 12 > m->m_nelen)
		return 0;
	shift = NEW(m->m_ne + NEW(m->m_ne + 0x24));
	off = (u32)NEW(m->m_ne + h) << shift;
	len = (u32)NEW(m->m_ne + h + 2) << shift;
	if ((g = g_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, len ? len : 1, m->m_hmod)) == 0)
		return 0;
	if ((fp = fopen(m->m_host, "rb")) != 0) {
		rdat(fp, off, M + sel_base(g), len);
		fclose(fp);
	}
	return g;
}

/* a loaded copy, kept: for our own use (menus, dialogs, strings) */
u32
res_data(m, type, name, sizep)
	struct module *m;
	u32 type, name, *sizep;
{
	u32 h = res_find(m, type, name);
	u16 g;

	if (!h)
		return 0;
	if ((g = res_load(m, h)) == 0)
		return 0;
	if (sizep)
		*sizep = g_size(g);
	return sel_base(g);
}
