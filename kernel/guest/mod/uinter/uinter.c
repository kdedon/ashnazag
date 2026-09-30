/*
 * uinter.c -- /dev/uinter0: the A/UX user-interface device as seen by
 * libmac1_s and Patch.067C.  One layer (the Mac environment), created
 * by startmac:
 *
 *	GETVERSION, TEST, UNMAP, MAP(0x3000), CREATELAYER, SHMID,
 *	ATTACHGFD, PHYS_SCREENS, ROM(0x40800000), SET(1)
 *
 * after which the task's A-line traps go to its low-memory vector $28
 * and its privileged instructions run against the virtual SR.  The
 * layer ends when its task exits or execs.
 *
 * Commands are A/UX's, passed unchanged by the personality's ioctl:
 * group 'Q', BSD direction bits and size; _IO commands take a value.
 * The screen is a slot $E card over a display session, whose keyboard
 * and mouse feed the event queue.  The rest of the A/UX set answers as
 * a machine without drives or SCSI would.
 *
 * K&R C.
 */

#include "uinter.h"
#include "sys/open.h"
#include "sys/sysmacros.h"
#include "sys/ipc.h"
#include "sys/shm.h"
#include "sys/resource.h"

extern int nodev(), ttimeout(), untimeout();
extern void (*aux_macdetach)(), (*aux_uitick)();
extern struct shminfo shminfo;

struct uilayer ui;
static int ui_nopen;
static int uiflag[1] = { 0 };
int	uinter_trace = 0;	/* 1: print each ioctl */
char	uinter_pram[64] = "/etc/aux/pram";	/* XPRAM across sessions */
static int ui_pramdirty;

/*
 * XPRAM $08-$1F after a PRAM reset: the last four bytes of the system
 * parameters (double-click and caret time $88, among them), 'NuMc',
 * then the first sixteen (valid $A8, serial ports, font, keyboard).
 * $76: default startup device.
 */
static unsigned char pram_sys[24] = {
	0x13, 0x88, 0x00, 0x4c, 'N', 'u', 'M', 'c',
	0xa8, 0x00, 0x00, 0x00, 0xcc, 0x0a, 0xcc, 0x0a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00
};
static unsigned char pram_76[6] = { 0x00, 0x01, 0xff, 0xff, 0xff, 0xdf };
static int ui_tcmd, ui_te, ui_tpid;

/* the caller's A/UX state, 0 for a native process */
static struct guest_proc *
auxgp()
{
	struct guest_proc *gp = GUESTP(u.u_procp);

	return gp && gp->gp_prof == &aux_profile ? gp : 0;
}

/* ---- the tick: SIGIOT to the task each clock tick (UI_TIMER) ---- */

static void
uitick(arg)
	caddr_t arg;
{
	ui.l_tid = 0;
	if (ui.l_state != LS_INUSE || ui.l_proc == 0)
		return;
	ui_evtick();
	psignal(ui.l_proc, SIGIOT);
	wakeup((caddr_t)&ui.l_tid);
	if ((ui.l_tid = ttimeout(uitick, (caddr_t)0, 1L)) == -1)
		ui.l_tid = 0;
}

static void
uitickstop()
{
	int s = splhi_();

	if (ui.l_tid) {
		untimeout(ui.l_tid);
		ui.l_tid = 0;
	}
	splx_(s);
}

/* ---- XPRAM: read from its file as a session starts, written back as it ends ---- */

/* a Mac's PRAM after a reset */
static void
pramreset()
{
	bzero((caddr_t)ui.l_pram, sizeof ui.l_pram);
	bcopy((caddr_t)pram_sys, (caddr_t)ui.l_pram + 0x08, sizeof pram_sys);
	bcopy((caddr_t)pram_76, (caddr_t)ui.l_pram + 0x76, sizeof pram_76);
	/* 32-bit addressing: 24-bit mode needs the low 16 MB mirrored across the space */
	ui.l_pram[0x8a] = 5;
}

/* no file, or a short one: the reset contents */
static void
prampin()
{
	struct vnode *vp;
	unsigned char b[sizeof ui.l_pram];
	int resid = 0;

	pramreset();
	if (lookupname(uinter_pram, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp))
		return;
	if (vp->v_type == VREG && VOP_ACCESS(vp, VREAD, 0, u.u_cred) == 0 && vn_rdwr(UIO_READ, vp, (caddr_t)b, (int)sizeof b, (off_t)0,
	    UIO_SYSSPACE, 0, 0x7fffffffL, u.u_cred, &resid) == 0 && resid == 0) {
		bcopy((caddr_t)b, (caddr_t)ui.l_pram, sizeof b);
		ui.l_pram[0x8a] |= 5;
	}
	VN_RELE(vp);
}

/* as root, whoever ends the session; only a regular file, not through a link */
static void
pramout()
{
	struct vnode *vp;
	struct cred *cr;
	int resid = 0, e;

	if (!ui_pramdirty)
		return;
	ui_pramdirty = 0;
	cr = crget();
	e = lookupname(uinter_pram, UIO_SYSSPACE, NO_FOLLOW, NULLVPP, &vp);
	if (e == 0) {
		if (vp->v_type != VREG)
			e = EINVAL;
		VN_RELE(vp);
	} else if (e == ENOENT)
		e = 0;
	if (e == 0 && (e = vn_open(uinter_pram, UIO_SYSSPACE, FWRITE | FCREAT | FTRUNC, 0644,
	    &vp, CRCREAT)) == 0) {
		if (vp->v_type != VREG)
			e = EINVAL;
		else
			e = vn_rdwr(UIO_WRITE, vp, (caddr_t)ui.l_pram, (int)sizeof ui.l_pram,
			    (off_t)0, UIO_SYSSPACE, 0, 0x7fffffffL, cr, &resid);
		VOP_CLOSE(vp, FWRITE, 1, (off_t)0, cr);
		VN_RELE(vp);
	}
	crfree(cr);
	if (e || resid)
		printf("uinter: cannot write %s (%d)\n", uinter_pram, e);
}

/*
 * The task or the ui page's holder leaves (exit, exec): the ui page is
 * free; for the task, no more ticks or reflection and the layer is free
 * for the next startmac.  Its mappings go with the address space.
 */
static void
uidetach(gp)
	struct guest_proc *gp;
{
	if (ui.l_uiproc == gp->gp_proc) {
		ui.l_uip = 0;
		ui.l_uiproc = 0;
	}
	AUXP(gp)->ap_mac &= ~APM_UIP;
	if (ui.l_gp != gp)
		return;
	uitickstop();
	ui_unscreen(0);
	ui_inreset();
	gp->gp_flags &= ~(GPF_ALINE | GPF_PRIV | GPF_SPIN | GPF_VPEND | GPF_FAULT);
	AUXP(gp)->ap_mac = 0;
	pramout();
	ui.l_state = LS_EMPTY;
	ui.l_proc = 0;
	ui.l_gp = 0;
	ui.l_shmid = -1;
	ui.l_romaddr = 0;
	ui.l_romproc = 0;
}

/* ---- entry points ---- */

int
uiopen(devp, flag, otyp, cr)
	dev_t *devp;
	int flag, otyp;
	struct cred *cr;
{
	if (getminor(*devp) != 0)
		return ENXIO;
	ui_nopen++;
	return 0;
}

int
uiclose(dev, flag, otyp, cr)
	dev_t dev;
	int flag, otyp;
	struct cred *cr;
{
	ui_nopen = 0;
	return 0;
}

int
uirdwr(dev, uiop, cr)
	dev_t dev;
	struct uio *uiop;
	struct cred *cr;
{
	return EINVAL;
}

/* the value of an _IO command, or its argument in b (IN) */
static int
uiarg(cmd, arg, b)
	int cmd, arg;
	char *b;
{
	int n = UIOC_SIZE(cmd);

	if (!(cmd & UIOC_IN) || n == 0)
		return 0;
	return copyin((caddr_t)arg, b, n) ? EFAULT : 0;
}

/* ROM, ui page, low memory: startmac's part */
static int
uisetup(gp, cmd, arg, b, rvp)
	struct guest_proc *gp;
	int cmd, arg;
	char *b;
	int *rvp;
{
	struct proc *p = u.u_procp;
	char f[UI_UIPSIZE / 64];
	long a, n;
	int e;

	switch (UIOC_NUM(cmd)) {
	case 1:				/* UI_SET: Mac vectors on */
		if (ui.l_gp != gp)
			return EINVAL;
		gp->gp_vsr = 0x2000;
		gp->gp_vvbr = 0;
		gp->gp_flags |= GPF_ALINE | GPF_PRIV | GPF_SPIN;
		return 0;
	case 2:				/* UI_CLEAR */
		if (ui.l_gp != gp)
			return EINVAL;
		gp->gp_flags &= ~(GPF_ALINE | GPF_PRIV | GPF_SPIN | GPF_VPEND | GPF_FAULT);
		gp->gp_vsr = 0;
		return 0;
	case 5:				/* UI_ROM(addr) */
		if (ui.l_gp != gp || ui.l_romaddr)
			return EINVAL;
		if ((e = ui_rommap((caddr_t)arg)) != 0)
			return e;
		ui.l_romaddr = (caddr_t)arg;
		ui.l_romproc = p;
		return 0;
	case 6:				/* UI_UNROM */
		if (ui.l_romaddr == 0 || ui.l_romproc != p)
			return EINVAL;
		e = ui_romunmap(ui.l_romaddr);
		ui.l_romaddr = 0;
		ui.l_romproc = 0;
		return e;
	case 7:				/* UI_MAP(uaddr) */
		if (ui.l_uip || (arg & (UI_UIPSIZE - 1)) ||
		    !valid_usr_range((caddr_t)arg, UI_UIPSIZE))
			return EINVAL;
		/* struct ui_interface: positions 0, mouse button up */
		bzero(f, sizeof f);
		if (copyout(f, (caddr_t)arg, 16) || subyte((caddr_t)arg + 0x458, 0x80))
			return EFAULT;
		ui.l_uip = (caddr_t)arg;
		ui.l_uiproc = p;
		AUXP(gp)->ap_mac |= APM_UIP;
		return 0;
	case 8:				/* UI_UNMAP */
		if (ui.l_uiproc == p && ui.l_gp != gp) {
			ui.l_uip = 0;
			ui.l_uiproc = 0;
			AUXP(gp)->ap_mac &= ~APM_UIP;
		}
		return 0;
	case 21:			/* UI_CREATELAYER */
		if (ui.l_state != LS_EMPTY)
			return EEXIST;
		if (ui.l_uiproc != p)
			return EINVAL;
		ui.l_state = LS_INUSE;
		ui.l_proc = p;
		ui.l_gp = gp;
		ui.l_shmid = -1;
		ui.l_evmask = 0xffef;
		AUXP(gp)->ap_mac |= APM_TASK;
		prampin();
		*rvp = 0;
		return 0;
	case 32:			/* UI_COPY_OUT {start, count} */
		a = G32(b);
		n = G32(b + 4);
		if ((e = ui_romload()) != 0)
			return e;
		if (a < 0 || n < 0 || a > UI_LOWSIZE || n > UI_LOWSIZE - a)
			return EINVAL;
		return copyout((caddr_t)ui_rom.r_low + a, (caddr_t)a, n) ? EFAULT : 0;
	case 67:			/* UI_GET_PRODINFO(ptr) */
		if ((e = ui_romload()) != 0)
			return e;
		if (ui_rom.r_prodoff < 0)
			return EINVAL;
		return copyout((caddr_t)ui_rom.r_prod, (caddr_t)arg, UI_PRODSIZE) ?
		    EFAULT : 0;
	}
	return ENOTTY;
}

/*
 * Everything else startmac and Patch.067C issue: events; no drives,
 * SCSI or VM; PRAM in memory.
 */
static int
uimisc(gp, cmd, arg, b, rvp)
	struct guest_proc *gp;
	int cmd, arg;
	char *b;
	int *rvp;
{
	struct rlimit *lim;
	long off, n;
	int i;

	switch (UIOC_NUM(cmd)) {
	case 4:				/* UI_UNSCREEN */
		if (ui.l_gp == gp)
			ui_unscreen(1);
		return ui.l_state == LS_INUSE ? 0 : EINVAL;
	case 9: case 10:		/* UI_CURSOR, UI_UNCURSOR */
		if (ui.l_gp != gp)
			return EINVAL;
		ui_inflags(IN_CUR, UIOC_NUM(cmd) == 9);
		return 0;
	case 24: case 25:		/* UI_DEVICES, UI_UNDEVICES */
		if (ui.l_gp != gp)
			return EINVAL;
		ui_inflags(IN_DEV, UIOC_NUM(cmd) == 24);
		return 0;
	case 16: case 30: case 55:	/* UI_POSTEVENT, UI_POST_MOD, UI_POST_EVTREC */
		if (ui.l_gp != gp)
			return EINVAL;
		return ui_postevent(b, UIOC_NUM(cmd) == 16 ? 0 : UIOC_NUM(cmd) == 30 ? 1 : 2);
	case 18:			/* UI_FLUSHEVENTS */
		if (ui.l_gp != gp)
			return EINVAL;
		ui_flushevents(b);
		return 0;
	case 23:			/* UI_SETLAYER */
	case 26:			/* UI_SETSELRECT */
	case 57: case 58: case 59: case 60: case 61:	/* VM holds and locks */
		return ui.l_state == LS_INUSE ? 0 : EINVAL;
	case 20:			/* UI_SETEVENTMASK */
		if (ui.l_gp != gp)
			return EINVAL;
		ui.l_evmask = G16(b);
		return 0;
	case 12:			/* UI_DELAY(Ticks to wait for): ticks since boot */
		off = G32(b);
		while (ui.l_tid && ui.l_gp == gp && (long)(lbolt - off) < 0)
			if (sleep((caddr_t)&ui.l_tid, (PZERO + 1) | PCATCH))
				return EINTR;
		P32(b, lbolt);
		return 0;
	case 19:			/* UI_GETOSEVENT */
		if (ui.l_gp != gp)
			return EINVAL;
		*rvp = 0;
		return ui_getosevent(b);
	case 28: case 29:		/* UI_READPRAM, UI_WRITEPRAM */
		off = G32(b + 4) & 0xffff;
		n = (G32(b + 4) >> 16) & 0xffff;
		if (off + n > sizeof ui.l_pram)
			return EINVAL;
		if (UIOC_NUM(cmd) == 28)
			return copyout((caddr_t)ui.l_pram + off, (caddr_t)G32(b), n) ?
			    EFAULT : 0;
		if (!suser(u.u_cred))
			return EPERM;
		ui_pramdirty = 1;
		return copyin((caddr_t)G32(b), (caddr_t)ui.l_pram + off, n) ? EFAULT : 0;
	case 34:			/* UI_PHYS_SCREENS */
		if (ui.l_state != LS_INUSE)
			return EINVAL;
		for (i = 0; i < 6; i++) {
			P8(b + 8 * i, 0xff);
			P8(b + 8 * i + 1, 0);
			P8(b + 8 * i + 2, 0);
			P8(b + 8 * i + 3, 0);
			P32(b + 8 * i + 4, 0);
		}
		return ui.l_gp == gp ? ui_screens(b) : 0;
	case 35:			/* UI_TIMER: start the tick */
		if (ui.l_gp != gp)
			return EINVAL;
		if (ui.l_tid == 0 && (ui.l_tid = ttimeout(uitick, (caddr_t)0, 1L)) == -1) {
			ui.l_tid = 0;
			return EAGAIN;
		}
		return 0;
	case 38:			/* UI_SWITCH(pid): one task, so only to itself */
		if (ui.l_gp != gp || (G32(b) != u.u_procp->p_pid && (long)G32(b) >= 0))
			return EINVAL;
		if ((long)G32(b) >= 0 && ui.l_uiproc == u.u_procp &&
		    suword(ui.l_uip + 0xff8, (int)G32(b)))
			return EFAULT;
		return 0;
	case 39:			/* UI_SLEEP */
		return ui.l_gp == gp ? 0 : EINVAL;
	case 36:			/* UI_ATTACHGFD: room for the global fds */
		if (ui.l_gp != gp)
			return EINVAL;
		lim = &u.u_rlimit[RLIMIT_NOFILE];
		if (lim->rlim_cur < AUX_GFD + AUX_GNOFILE)
			lim->rlim_cur = lim->rlim_max < AUX_GFD + AUX_GNOFILE ?
			    lim->rlim_max : AUX_GFD + AUX_GNOFILE;
		return 0;
	case 42:			/* UI_GETDQEL: no drives, the end of the list */
		return EINVAL;
	case 46:			/* UI_SHMID */
		if (ui.l_gp != gp)
			return EINVAL;
		ui.l_shmid = G32(b);
		return 0;
	case 47: case 48:		/* UI_VIDEO_CONTROL, _STATUS */
		if (ui.l_gp != gp)
			return EINVAL;
		return ui_video(UIOC_NUM(cmd) == 47, b);
	case 50:			/* UI_SYNC */
		return ui.l_state == LS_INUSE ? 0 : EAGAIN;
	case 51:			/* UI_SET_KCHR */
		if (ui.l_gp != gp)
			return EINVAL;
		n = G32(b + 4);
		if (n < 0 || n > sizeof ui.l_kchr)
			return EINVAL;
		ui.l_kchrlen = 0;
		if (copyin((caddr_t)G32(b), (caddr_t)ui.l_kchr, n))
			return EFAULT;
		ui.l_kchrlen = n;
		return 0;
	case 52:			/* UI_TEST */
		return ui.l_state == LS_INUSE ? EEXIST : 0;
	case 56:			/* UI_GETKIFLAGS */
		P16(b, 0);
		return 0;
	case 62:			/* UI_VM_GETPHYS */
		return EINVAL;
	case 63:			/* UI_SETCOFFNAME {id, path, len} */
		n = (short)G16(b + 8);
		if (n < 0 || n >= sizeof ui.l_coffname)
			return EINVAL;
		if (copyin((caddr_t)G32(b + 4), ui.l_coffname, n))
			return EFAULT;
		ui.l_coffname[n] = 0;
		ui.l_coffnamelen = n;
		ui.l_coffid = G32(b);
		return 0;
	case 64:			/* UI_GETCOFFNAME */
		n = (short)G16(b + 8);
		if (n > ui.l_coffnamelen)
			n = ui.l_coffnamelen;
		if (n < 0 || copyout(ui.l_coffname, (caddr_t)G32(b + 4), n))
			return EFAULT;
		P16(b + 8, n);
		return 0;
	case 68:			/* UI_GET_INTERR_VECTORS: none saved */
		*rvp = 1;
		return 0;
	case 69:			/* UI_GETKEYS */
		return ui.l_gp == gp ? ui_getkeys((caddr_t)arg) : EINVAL;
	case 70:			/* UI_GETSCSIID: none usable */
		{
			char s[0x1c];

			bzero(s, sizeof s);
			return copyout(s, (caddr_t)arg, sizeof s) ? EFAULT : 0;
		}
	case 27:			/* UI_HASKIDS */
		return ECHILD;
	case 43:			/* UI_KILLMYLAYER */
		if (ui.l_gp == gp)
			uidetach(gp);
		return 0;
	case 44: case 45:		/* UI_REBOOT, UI_SHUTDOWN */
		return EPERM;
	}
	return EINVAL;	/* 11, 17, 22, 31, 33, 37, 40, 41, 53, 54, 65, 66, 71: not here */
}

int
uiioctl(dev, cmd, arg, mode, cr, rvp)
	dev_t dev;
	int cmd, arg, mode;
	struct cred *cr;
	int *rvp;
{
	struct guest_proc *gp = auxgp();
	char b[64];
	int e, n = UIOC_SIZE(cmd);

	*rvp = 0;
	if ((cmd & 0xff00) != UIOC_GROUP || n > sizeof b)
		return EINVAL;
	if (UIOC_NUM(cmd) == 0) {	/* UI_GETVERSION */
		*rvp = UI_VERSION;
		return 0;
	}
	if (gp == 0)
		return EINVAL;		/* the Mac side is an A/UX process */
	bzero(b, sizeof b);
	if ((e = uiarg(cmd, arg, b)) != 0)
		return e;
	e = uisetup(gp, cmd, arg, b, rvp);
	if (e == ENOTTY)
		e = uimisc(gp, cmd, arg, b, rvp);
	if (uinter_trace && (cmd != ui_tcmd || e != ui_te || u.u_procp->p_pid != ui_tpid)) {
		char t[8];

		ui_tcmd = cmd;		/* a polled call shows once */
		ui_te = e;
		ui_tpid = u.u_procp->p_pid;
		t[0] = 'Q';
		t[1] = '0' + UIOC_NUM(cmd) / 10;
		t[2] = '0' + UIOC_NUM(cmd) % 10;
		t[3] = 0;
		aux_tlog((int)u.u_procp->p_pid, t, (long)-e);
	}
	if (e == 0 && (cmd & UIOC_OUT) && n &&
	    copyout(b, (caddr_t)arg, n))
		e = EFAULT;
	return e;
}

/* ---- module ---- */

static int
uinter_load()
{
	/* Mac RAM (tLOW) is up to 16 MB plus 4 MB */
	if (shminfo.shmmax < 0x1400000)
		shminfo.shmmax = 0x1400000;
	aux_macdetach = uidetach;
	aux_uitick = ui_update;
	aux_slotmgr = ui_slotmgr;
	ui.l_shmid = -1;
	pramreset();
	return 0;
}

static int
uinter_unload()
{
	if (ui.l_state != LS_EMPTY || ui_nopen)
		return EBUSY;
	aux_macdetach = 0;
	aux_uitick = 0;
	aux_slotmgr = 0;
	ui_romfree();
	return 0;
}

struct mod_drv_data uinter_drvdata[] = {
	{ { nodev, nodev, nodev, nodev, nodev, nodev, nodev, 0 }, 0, 0,
	  { uiopen, uiclose, uirdwr, uirdwr, uiioctl, nodev, nodev, nodev,
	    nodev, nodev, 0, 0, uiflag }, UI_MAJOR, 1 }
};

MOD_DRV_WRAPPER(uinter, uinter_load, uinter_unload, 0, "A/UX user interface");
