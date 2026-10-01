/*
 * uivideo.c -- the Mac screen: a NuBus card in slot $E whose frame
 * buffer is a display-service session mapped at the start of its super
 * slot space ($E0000000, clear of the kernel's addresses), its
 * declaration ROM (kept here, read through the Slot Manager call), and
 * the video driver's Control and Status calls.
 *
 * Declaration ROM, all byte lanes, ending at the top of the (unmapped)
 * minor slot space:
 *
 *	directory: 1 board, $80 video
 *	video sRsrc: type display/video, name, flags, minor base and
 *	    length, mode $80 = the display's current depth (VPBlock,
 *	    page count 1, device type)
 *	format block
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/open.h"
#include "sys/poll.h"
#include "sys/mman.h"
#include "vm/seg.h"
#include "vm/as.h"
#include "dsio.h"
#define	printf	ds_printf	/* ds.h declares it int */
#include "ds.h"
#undef	printf

extern struct vnode *makespecvp();

#define	UV_SLOT		0xE
#define	UV_SLOTVA	0xE0000000
#define	UV_DECLSIZE	0x100
#define	UV_DECLVA	(0xFF000000 - UV_DECLSIZE)
#define	UV_BOARDID	0x7F00
#define	UV_MODE		0x80

/* Slot Manager results */
#define	SM_EMPTY	(-300)
#define	SM_BADREF	(-330)
#define	SM_SELOOB	(-338)
#define	SM_NOMORE	(-344)
#define	SM_GETDRVR	(-345)
#define	SM_BADPTR	(-346)
#define	SM_NOTFOUND	(-351)
#define	SM_NOSINFO	(-306)

/* SpBlock */
#define	SP_RESULT	0
#define	SP_PTR		4
#define	SP_SIZE		8
#define	SP_OFFDATA	12
#define	SP_IORES	36
#define	SP_REFNUM	38
#define	SP_CAT		40
#define	SP_TBMASK	48
#define	SP_SLOT		49
#define	SP_ID		50
#define	SP_EXTDEV	51
#define	SP_HWDEV	52
#define	SP_LANES	53
#define	SP_FLAGS	54
#define	SP_LEN		56

struct uivideo {
	int		v_on;
	struct vnode	*v_vp;		/* open display file */
	struct dssess	*v_sess;
	struct proc	*v_proc;	/* whose address space has the map */
	u_int		v_len;
	unsigned long	v_base;		/* first pixel */
	int		v_depth, v_width, v_height, v_rowbytes;
	int		v_gray;
	short		v_refnum, v_iores;	/* sUpdateSRT of the video sRsrc */
	unsigned char	v_decl[UV_DECLSIZE];
};
static struct uivideo uv;

/* ---- declaration ROM ---- */

static void
uv_ent(p, id, v)
	int p, id;
	long v;
{
	P8(uv.v_decl + p, id);
	P8(uv.v_decl + p + 1, v >> 16);
	P16(uv.v_decl + p + 2, v);
}

/* entry at p pointing at q */
static void
uv_off(p, id, q)
	int p, id, q;
{
	uv_ent(p, id, (long)(q - p));
}

static void
uv_str(p, s)
	int p;
	char *s;
{
	while ((uv.v_decl[p++] = *s++) != 0)
		;
}

static void
uv_mkdecl(fi)
	struct fbinfo *fi;
{
	unsigned char *d = uv.v_decl;
	int direct = fi->fi_depth > 8, fh = UV_DECLSIZE - 20;

	bzero((caddr_t)d, UV_DECLSIZE);
	uv_off(0x00, 0x01, 0x10);		/* directory */
	uv_off(0x04, UV_MODE, 0x40);
	uv_ent(0x08, 0xff, 0L);
	uv_off(0x10, 1, 0x20);			/* board: type, name, id */
	uv_off(0x14, 2, 0x28);
	uv_ent(0x18, 0x20, (long)UV_BOARDID);
	uv_ent(0x1c, 0xff, 0L);
	P16(d + 0x20, 1);			/* catBoard */
	uv_str(0x28, "A/UX Screen");
	uv_off(0x40, 1, 0x60);			/* video sRsrc */
	uv_off(0x44, 2, 0x68);
	uv_ent(0x48, 7, 6L);			/* open at start, 32-bit base */
	uv_ent(0x4c, 8, 1L);			/* hardware device id */
	uv_off(0x50, 0x0a, 0x80);		/* minor base */
	uv_off(0x54, 0x0b, 0x84);		/* minor length */
	uv_off(0x58, UV_MODE, 0x88);
	uv_ent(0x5c, 0xff, 0L);
	P16(d + 0x60, 3);			/* catDisplay, typVideo, drSwApple */
	P16(d + 0x62, 1);
	P16(d + 0x64, 1);
	P16(d + 0x66, 0x7a);
	uv_str(0x68, "Display_Video_AUX");
	P32(d + 0x80, uv.v_base - UV_SLOTVA);
	P32(d + 0x84, uv.v_len);
	uv_off(0x88, 1, 0x98);			/* mode: VPBlock, pages, type */
	uv_ent(0x8c, 3, 1L);
	uv_ent(0x90, 4, direct ? 2L : (fi->fi_flags & FBF_CMAP) ? 0L : 1L);
	uv_ent(0x94, 0xff, 0L);
	P32(d + 0x98, 4 + 0x2a);		/* VPBlock, with its length */
	P16(d + 0xa0, fi->fi_rowbytes);		/* after the base offset */
	P16(d + 0xa6, fi->fi_height);		/* bounds 0, 0, height, width */
	P16(d + 0xa8, fi->fi_width);
	P32(d + 0xb2, 0x480000L);		/* 72 dpi */
	P32(d + 0xb6, 0x480000L);
	P16(d + 0xba, direct ? 16 : 0);		/* pixel type */
	P16(d + 0xbc, fi->fi_depth);
	P16(d + 0xbe, direct ? 3 : 1);
	P16(d + 0xc0, direct ? (fi->fi_depth == 16 ? 5 : 8) : fi->fi_depth);
	/* format block: directory offset, length, revision, format, test pattern, lanes */
	P32(d + fh, (0 - fh) & 0xffffff);
	P32(d + fh + 4, UV_DECLSIZE);
	P8(d + fh + 12, 1);
	P8(d + fh + 13, 1);
	P32(d + fh + 14, 0x5a932bc7);
	P8(d + fh + 19, 0x0f);
}

/* image offset of a Mac pointer, -1 outside */
static int
uv_at(a)
	unsigned long a;
{
	return a >= UV_DECLVA && a < UV_DECLVA + UV_DECLSIZE ? (int)(a - UV_DECLVA) : -1;
}

/* entry id in the list at image offset l, -1 none */
static int
uv_find(l, id)
	int l, id;
{
	for (; l >= 0 && l + 4 <= UV_DECLSIZE && uv.v_decl[l] != 0xff; l += 4)
		if (uv.v_decl[l] == id)
			return l;
	return -1;
}

static long
uv_data(e)
	int e;
{
	return G32(uv.v_decl + e) & 0xffffff;
}

/* target of entry e, -1 outside */
static int
uv_tgt(e)
	int e;
{
	long o = uv_data(e);

	if (o & 0x800000)
		o -= 0x1000000;
	o += e;
	return o >= 0 && o < UV_DECLSIZE ? (int)o : -1;
}

/* the sRsrc list of id in slot $E, -1 none */
static int
uv_rsrc(slot, id)
	int slot, id;
{
	int e;

	if (!uv.v_on || slot != UV_SLOT || (e = uv_find(0, id)) < 0)
		return -1;
	return uv_tgt(e);
}

/* sRsrcInfo's outputs for sRsrc id at l */
static void
uv_info(b, id, l)
	char *b;
	int id, l;
{
	int e = uv_tgt(uv_find(l, 1));

	P32(b + SP_PTR, UV_DECLVA + l);
	P8(b + SP_SLOT, UV_SLOT);
	P8(b + SP_ID, id);
	P8(b + SP_EXTDEV, 0);
	P16(b + SP_REFNUM, id == UV_MODE ? uv.v_refnum : 0);
	P16(b + SP_IORES, id == UV_MODE ? uv.v_iores : 0);
	bcopy((caddr_t)uv.v_decl + e, b + SP_CAT, 8);
	e = uv_find(l, 8);
	P8(b + SP_HWDEV, e < 0 ? 0 : uv_data(e));
}

/* copy n bytes of the image from o to the user, or report the size */
static int
uv_give(b, o, n)
	char *b;
	int o, n;
{
	if (o < 0 || n < 0 || n > UV_DECLSIZE || o > UV_DECLSIZE - n)
		return SM_BADREF;
	P32(b + SP_SIZE, n);
	if (G32(b + SP_RESULT) && copyout((caddr_t)uv.v_decl + o, (caddr_t)G32(b + SP_RESULT), n))
		return EFAULT;
	return 0;
}

/* next sRsrc after (slot, id); typed: matching the category fields not masked */
static int
uv_next(b, typed)
	char *b;
	int typed;
{
	static int ids[] = { 1, UV_MODE };
	char want[8];
	int i, k, l, m = G8(b + SP_TBMASK);

	bcopy(b + SP_CAT, want, 8);
	for (i = 0; i < 2; i++) {
		if (G8(b + SP_SLOT) > UV_SLOT ||
		    (G8(b + SP_SLOT) == UV_SLOT && G8(b + SP_ID) >= ids[i]))
			continue;
		if ((l = uv_rsrc(UV_SLOT, ids[i])) < 0)
			continue;
		uv_info(b, ids[i], l);
		if (!typed)
			return 0;
		for (k = 0; k < 4; k++)
			if (!(m & (8 >> k)) && G16(b + SP_CAT + 2 * k) != G16(want + 2 * k))
				break;
		if (k == 4)
			return 0;
		bcopy(want, b + SP_CAT, 8);
	}
	return SM_NOMORE;
}

/* one Slot Manager selector on the copied-in SpBlock: an OSErr, or EFAULT */
static int
uv_slot(sel, b)
	int sel;
	char *b;
{
	int slot = G8(b + SP_SLOT), id = G8(b + SP_ID);
	int l = uv_at(G32(b + SP_PTR)), e, n;
	unsigned char *d = uv.v_decl;
	char r[24];

	switch (sel) {
	case 0x00: case 0x01:		/* sReadByte, sReadWord: the entry's data */
		if ((e = uv_find(l, id)) < 0)
			return SM_BADREF;
		P32(b + SP_RESULT, uv_data(e) & (sel ? 0xffff : 0xff));
		return 0;
	case 0x02:			/* sReadLong */
		if ((e = uv_tgt(uv_find(l, id))) < 0 || e + 4 > UV_DECLSIZE)
			return SM_BADREF;
		P32(b + SP_RESULT, G32(d + e));
		return 0;
	case 0x03:			/* sGetcString */
		if ((e = uv_tgt(uv_find(l, id))) < 0)
			return SM_BADREF;
		for (n = 0; e + n < UV_DECLSIZE && d[e + n]; n++)
			;
		return uv_give(b, e, n + 1);
	case 0x05:			/* sGetBlock: after its length long */
		if ((e = uv_tgt(uv_find(l, id))) < 0 || e + 4 > UV_DECLSIZE)
			return SM_BADREF;
		return uv_give(b, e + 4, (int)G32(d + e) - 4);
	case 0x06:			/* sFindStruct */
		if ((e = uv_tgt(uv_find(l, id))) < 0)
			return SM_BADREF;
		P32(b + SP_PTR, UV_DECLVA + e);
		return 0;
	case 0x07:			/* sReadStruct */
		return uv_give(b, l, (int)G32(b + SP_SIZE));
	case 0x10:			/* sReadInfo */
		if (!uv.v_on || slot != UV_SLOT)
			return SM_EMPTY;
		bzero(r, sizeof r);
		P32(r, UV_DECLVA);		/* directory */
		P8(r + 9, 0x0f);		/* byte lanes */
		P8(r + 10, 0xff);		/* top of ROM */
		P16(r + 12, 0x100);
		P32(r + 16, UV_DECLVA + UV_DECLSIZE - 1);
		P8(r + 20, UV_SLOT);
		return copyout(r, (caddr_t)G32(b + SP_RESULT), 24) ? EFAULT : 0;
	case 0x11:			/* sReadPRAMRec */
		if (!uv.v_on || slot != UV_SLOT)
			return SM_EMPTY;
		bcopy((caddr_t)ui.l_pram + 0x48 + 8 * (UV_SLOT - 9), r, 8);
		P16(r, UV_BOARDID);
		return copyout(r, (caddr_t)G32(b + SP_RESULT), 8) ? EFAULT : 0;
	case 0x12:			/* sPutPRAMRec: the vendor bytes */
		if (!uv.v_on || slot != UV_SLOT)
			return SM_EMPTY;
		if (uv.v_proc != u.u_procp)
			return EPERM;
		if (copyin((caddr_t)G32(b + SP_PTR), r, 8))
			return EFAULT;
		bcopy(r + 2, (caddr_t)ui.l_pram + 0x48 + 8 * (UV_SLOT - 9) + 2, 6);
		return 0;
	case 0x13:			/* sReadFHeader */
		if (!uv.v_on || slot != UV_SLOT)
			return SM_EMPTY;
		return copyout((caddr_t)d + UV_DECLSIZE - 20, (caddr_t)G32(b + SP_RESULT), 20) ?
		    EFAULT : 0;
	case 0x14: case 0x15:		/* sNextsRsrc, sNextTypesRsrc */
		return uv_next(b, sel == 0x15);
	case 0x16: case 0x30:		/* sRsrcInfo, sFindsRsrcPtr */
	case 0x2a:			/* sSearchSRT */
		if ((l = uv_rsrc(slot, id)) < 0)
			return SM_NOTFOUND;
		if (sel == 0x30)
			P32(b + SP_PTR, UV_DECLVA + l);
		else
			uv_info(b, id, l);
		return 0;
	case 0x18:			/* sCkCardStat */
		return uv.v_on && slot == UV_SLOT ? 0 : SM_EMPTY;
	case 0x19:			/* sReadDrvrName: '.' and the sRsrc name, Pascal */
		if ((l = uv_rsrc(slot, id)) < 0 || (e = uv_tgt(uv_find(l, 2))) < 0)
			return SM_NOTFOUND;
		for (n = 0; n < 20 && d[e + n]; n++)
			r[n + 2] = d[e + n];
		r[0] = n + 1;
		r[1] = '.';
		return copyout(r, (caddr_t)G32(b + SP_RESULT), n + 2) ? EFAULT : 0;
	case 0x1b: case 0x1c:		/* sFindDevBase, sFindBigDevBase */
		if ((l = uv_rsrc(slot, id)) < 0)
			return SM_NOTFOUND;
		e = uv_tgt(uv_find(l, 0x0a));
		P32(b + SP_RESULT, UV_SLOTVA + (e < 0 ? 0 : G32(d + e)));
		return 0;
	case 0x20: case 0x21: case 0x23: case 0x25: case 0x29:	/* inits, sExec */
		return 0;
	case 0x22:			/* sCardChanged */
		P32(b + SP_RESULT, 0L);
		return 0;
	case 0x24:			/* sOffsetData */
		if ((e = uv_find(l, id)) < 0)
			return SM_BADREF;
		P32(b + SP_OFFDATA, uv_data(e));
		P8(b + SP_LANES, 0x0f);
		return 0;
	case 0x26:			/* sReadPBSize */
		if ((e = uv_tgt(uv_find(l, id))) < 0 || e + 4 > UV_DECLSIZE)
			return SM_BADREF;
		P32(b + SP_SIZE, G32(d + e) & ((G8(b + SP_FLAGS) & 2) ? 0xffffffff : 0xffffff));
		return 0;
	case 0x28:			/* sCalcStep */
		P32(b + SP_RESULT, 1L);
		return 0;
	case 0x2b:			/* sUpdateSRT */
		if (uv_rsrc(slot, id) < 0)
			return SM_NOTFOUND;
		if (id == UV_MODE) {
			uv.v_refnum = G16(b + SP_REFNUM);
			uv.v_iores = G16(b + SP_IORES);
		}
		return 0;
	case 0x2c:			/* sCalcsPointer */
		P32(b + SP_PTR, G32(b + SP_PTR) + G32(b + SP_OFFDATA));
		return 0;
	case 0x2d:			/* sGetDriver */
		return SM_GETDRVR;
	case 0x2e:			/* sPtrToSlot */
		if ((G32(b + SP_PTR) >> 28) != 0xf)
			return SM_BADPTR;
		P8(b + SP_SLOT, G32(b + SP_PTR) >> 24);
		return 0;
	case 0x2f:			/* sFindsInfoRecPtr */
		return SM_NOSINFO;
	}
	return SM_SELOOB;
}

/* syscall 66 */
int
ui_slotmgr(sel, pb, resp)
	int sel;
	caddr_t pb;
	int *resp;
{
	char b[SP_LEN];
	int r;

	if (copyin(pb, b, SP_LEN))
		return EFAULT;
	r = uv_slot(sel, b);
	ui_note('S', sel, r);
	if (uinter_trace) {
		char t[8];

		t[0] = 'S';
		t[1] = "0123456789abcdef"[(sel >> 4) & 0xf];
		t[2] = "0123456789abcdef"[sel & 0xf];
		t[3] = 0;
		aux_tlog((int)u.u_procp->p_pid, t, (long)r);
	}
	if (r == EFAULT || r == EPERM)
		return r;
	*resp = (short)r;
	return copyout(b, pb, SP_LEN) ? EFAULT : 0;
}

/* ---- screens ---- */

/*
 * UI_PHYS_SCREENS for the task: a display session in front, its frame
 * buffer mapped at the slot base, the table's first entry.  No display:
 * no screens.  EPERM if the caller may not take the front.
 */
int
ui_screens(b)
	char *b;
{
	struct proc *p = u.u_procp;
	struct fbinfo *fi = &ds_disp.d_info;
	struct dssess *s;
	struct vnode *vp;
	addr_t a = (addr_t)UV_SLOTVA;
	int e;

	if (uv.v_on)
		return EINVAL;
	if (!ds_mayfront(u.u_cred))
		return EPERM;
	/* through the file system, as open and mmap would: the mapping holds the file */
	vp = makespecvp(makedevice(DS_FBMAJ, 0), VCHR);
	if (VOP_OPEN(&vp, FREAD | FWRITE, u.u_cred) != 0) {
		VN_RELE(vp);
		return 0;
	}
	if ((s = ds_newsess((long)u.u_cred->cr_uid, "mac", &e)) == 0)
		goto out;
	s->s_cache = FBC_CI;	/* the Mac never pushes the data cache after drawing */
	ds_fbh[getminor(vp->v_rdev) - 1].h_sess = s;
	e = VOP_MAP(vp, (off_t)0, p->p_as, &a, (u_int)fi->fi_size,
	    PROT_READ | PROT_WRITE | PROT_USER, PROT_ALL, MAP_SHARED | MAP_FIXED, u.u_cred);
	if (e)
		goto out;
	uv.v_on = 1;
	uv.v_vp = vp;
	uv.v_sess = s;
	uv.v_proc = p;
	uv.v_len = fi->fi_size;
	uv.v_base = UV_SLOTVA + fi->fi_offset;
	uv.v_depth = fi->fi_depth;
	uv.v_width = fi->fi_width;
	uv.v_height = fi->fi_height;
	uv.v_rowbytes = fi->fi_rowbytes;
	uv.v_gray = 0;
	uv.v_refnum = uv.v_iores = 0;
	uv_mkdecl(fi);
	ui_inscreen(uv.v_width, uv.v_height);
	s->s_kin = ui_kin;
	(void)ds_switch(s);
	P8(b, UV_SLOT);
	P8(b + 1, UV_MODE);
	P8(b + 2, 0);
	P32(b + 4, uv.v_base);
	return 0;
out:
	(void)VOP_CLOSE(vp, FREAD | FWRITE, 1, (off_t)0, u.u_cred);
	VN_RELE(vp);
	return e;
}

/* the session ends; the mapping, and the file with it, go now (unmap) or with the address space */
void
ui_unscreen(unmap)
	int unmap;
{
	if (!uv.v_on)
		return;
	uv.v_on = 0;
	if (unmap && uv.v_proc == u.u_procp)
		(void)as_unmap(u.u_procp->p_as, (addr_t)UV_SLOTVA, uv.v_len);
	uv.v_sess->s_kin = 0;
	ds_endsess(uv.v_sess);
	(void)VOP_CLOSE(uv.v_vp, FREAD | FWRITE, 1, (off_t)0, u.u_cred);
	VN_RELE(uv.v_vp);
	uv.v_vp = 0;
	uv.v_sess = 0;
	uv.v_proc = 0;
}

/* the Mac's view of the screen: its address in the task, row bytes, size, depth */
int
ui_scrgeom(bp, rbp, wp, hp, dp)
	caddr_t *bp;
	int *rbp, *wp, *hp, *dp;
{
	if (!uv.v_on || uv.v_proc != u.u_procp)
		return 1;
	*bp = (caddr_t)uv.v_base;
	*rbp = uv.v_rowbytes;
	*wp = uv.v_width;
	*hp = uv.v_height;
	*dp = uv.v_depth;
	return 0;
}

/* ---- the video driver ---- */

/* 50% grey: alternate pixels 0 and all ones, shifted each row */
static int
uv_graypage()
{
	unsigned char row[2][256];
	int d = uv.v_depth, y, x, n, i, bits;
	long w = (long)uv.v_width * d / 8;

	bzero((caddr_t)row, sizeof row);
	for (y = 0; y < 2; y++)
		for (i = 1 - y; i < (int)sizeof row[0] * 8 / d; i += 2)
			for (bits = 0; bits < d; bits++)
				row[y][(i * d + bits) >> 3] |= 0x80 >> ((i * d + bits) & 7);
	for (y = 0; y < uv.v_height; y++)
		for (x = 0; x < w; x += n) {
			n = w - x > sizeof row[0] ? sizeof row[0] : w - x;
			if (copyout((caddr_t)row[y & 1], (caddr_t)(uv.v_base + (long)y * uv.v_rowbytes + x), n))
				return EFAULT;
		}
	return 0;
}

/* SetEntries (put) or GetEntries on a VDSetEntryRecord */
static int
uv_entries(p, put)
	caddr_t p;
	int put;
{
	char r[8], cs[8];
	unsigned short cr, cg, cb, y;
	struct dssess *s = uv.v_sess;
	int i, start, n, k, max = ds_disp.d_info.fi_cmapsize;

	if (copyin(p, r, 8))
		return EFAULT;
	start = (short)G16(r + 4);
	n = (short)G16(r + 6) + 1;
	if (n <= 0 || n > 256 || (start >= 0 && start + n > max))
		return -50;
	for (i = 0; i < n; i++) {
		caddr_t c = (caddr_t)G32(r) + 8 * i;

		if (copyin(c, cs, 8))
			return EFAULT;
		k = start < 0 ? (int)G16(cs) : start + i;
		if (k < 0 || k >= max)
			return -50;
		if (!put) {
			P16(cs + 2, s->s_cmap[0][k]);
			P16(cs + 4, s->s_cmap[1][k]);
			P16(cs + 6, s->s_cmap[2][k]);
			if (copyout(cs, c, 8))
				return EFAULT;
			continue;
		}
		cr = G16(cs + 2);
		cg = G16(cs + 4);
		cb = G16(cs + 6);
		if (uv.v_gray) {
			y = ((unsigned long)cr * 30 + (unsigned long)cg * 59 +
			    (unsigned long)cb * 11) / 100;
			cr = cg = cb = y;
		}
		ds_setcmap(s, k, 1, &cr, &cg, &cb);
	}
	return 0;
}

/* VDPageInfo out: mode, page 0, base */
static int
uv_pageinfo(p)
	caddr_t p;
{
	char r[12];

	bzero(r, sizeof r);
	P16(r, UV_MODE);
	P32(r + 8, uv.v_base);
	return copyout(r, p, sizeof r) ? EFAULT : 0;
}

static int
uv_control(code, p)
	int code;
	caddr_t p;
{
	char r[4];

	switch (code) {
	case 0:				/* reset */
		return uv_pageinfo(p);
	case 1: case 4: case 7: case 9:	/* KillIO, SetGamma, SetInterrupt, SetDefaultMode */
		return 0;
	case 2:				/* SetMode: the one mode, page 0 */
		if (copyin(p, r, 2))
			return EFAULT;
		if (G16(r) != UV_MODE)
			return -17;
		return uv_pageinfo(p);
	case 3:				/* SetEntries */
		if (uv.v_depth > 8)
			return -17;
		return uv_entries(p, 1);
	case 5:				/* GrayPage */
		return uv_graypage();
	case 6:				/* SetGray */
		if (copyin(p, r, 1))
			return EFAULT;
		uv.v_gray = r[0] != 0;
		return 0;
	}
	return -17;
}

static int
uv_status(code, p)
	int code;
	caddr_t p;
{
	char r[4];

	switch (code) {
	case 2:				/* GetMode */
		return uv_pageinfo(p);
	case 3:				/* GetEntries */
		if (uv.v_depth > 8)
			return -18;
		return uv_entries(p, 0);
	case 4:				/* GetPages */
		P16(r, 1);
		return copyout(r, p + 6, 2) ? EFAULT : 0;
	case 5:				/* GetBaseAddr */
		P32(r, uv.v_base);
		return copyout(r, p + 8, 4) ? EFAULT : 0;
	case 6:				/* GetGray */
		r[0] = uv.v_gray;
		return copyout(r, p, 1) ? EFAULT : 0;
	case 7:				/* GetInterrupt: enabled */
		r[0] = 0;
		return copyout(r, p, 1) ? EFAULT : 0;
	case 9:				/* GetDefaultMode: the video sRsrc */
		r[0] = UV_MODE;
		return copyout(r, p, 1) ? EFAULT : 0;
	}
	return -18;
}

/* UI_VIDEO_CONTROL, _STATUS on the CntrlParam in b: slot at +4, result there */
int
ui_video(ctl, b)
	int ctl;
	char *b;
{
	int r;

	if (!uv.v_on || G16(b + 4) != UV_SLOT)
		r = -17;
	else if (ctl)
		r = uv_control((int)G16(b + 0x1a), (caddr_t)G32(b + 0x1c));
	else
		r = uv_status((int)G16(b + 0x1a), (caddr_t)G32(b + 0x1c));
	if (r == EFAULT)
		return EFAULT;
	P16(b + 4, r);
	return 0;
}
