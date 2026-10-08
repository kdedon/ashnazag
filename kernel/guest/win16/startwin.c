/*
 * startwin -- run a 16-bit Windows program as this process: our own
 * KERNEL, USER and GDI in native code, the program's x86 code
 * interpreted, on a Windows 3.1 style desktop of our own.
 *
 *	startwin [-C dir] [-D X=dir]... [-m MB] [-g WxH] [-v] [-S script]
 *		 [-o shot.ppm] [program [arguments]]
 *
 * Drive C: is dir (default ~/WIN16, made with WINDOWS, WINDOWS\SYSTEM
 * and TEMP in it), H: the home directory and R: the root, as Wabi had
 * them; -D adds others.  A program named without a drive is looked for
 * in the current directory, then C:\WINDOWS.  Without a program,
 * C:\WINDOWS\PROGMAN.EXE if it is there.  -m sets guest memory (8 MB),
 * -g the screen where the device lets us choose, -v more messages
 * (-vv every call), -S and -o drive the in-memory screen of host builds.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"
#include "scr.h"

static struct x86 thecpu;
struct x86 *cpu;
extern int w16_strict;
extern void initdeps();

/* ---- interrupts the program makes ---- */

#define	AX	(c->r[R_AX] & 0xffff)
#define	SETW(n, v)	(c->r[n] = (c->r[n] & ~0xffff) | ((v) & 0xffff))

u32 pmvec[256];		/* protected-mode interrupt vectors the program set */

static void
carry(c, on)
	struct x86 *c;
	int on;
{
	u32 f = x86_flags(c);

	x86_setflags(c, on ? f | F_CF : f & ~F_CF);
}

/* INT 31h: the DPMI calls Win16 programs make */
static int
dpmi(c)
	struct x86 *c;
{
	u32 bx = c->r[R_BX] & 0xffff, a, n;
	int s, i;
	struct desc *d;

	carry(c, 0);
	switch (AX) {
	case 0x0000:		/* allocate descriptors */
		n = c->r[R_CX] & 0xffff;
		s = sel_alloc(n ? n : 1);
		if (!s) {
			carry(c, 1);
			return 1;
		}
		for (i = 0; i < (int)n; i++)
			sel_set(s + 8 * i, 0, 0, D_P | D_S | D_W);
		SETW(R_AX, s);
		return 1;
	case 0x0001:		/* free */
		if (!g_block(bx))
			sel_free(bx, 1);
		return 1;
	case 0x0002:		/* segment to descriptor */
		s = sel_alloc(1);
		sel_set(s, bx << 4, 0xffff, D_P | D_S | D_W);
		SETW(R_AX, s);
		return 1;
	case 0x0003:
		SETW(R_AX, 8);
		return 1;
	case 0x0006:
		a = sel_base(bx);
		SETW(R_CX, a >> 16);
		SETW(R_DX, a);
		return 1;
	case 0x0007:
		LDT[SELIX(bx)].d_base = (c->r[R_CX] & 0xffff) << 16 | (c->r[R_DX] & 0xffff);
		for (i = 0; i < 6; i++)
			if (c->s[i].sel == bx)
				x86_loadseg(c, i, bx);
		return 1;
	case 0x0008:
		LDT[SELIX(bx)].d_limit = (c->r[R_CX] & 0xffff) << 16 | (c->r[R_DX] & 0xffff);
		for (i = 0; i < 6; i++)
			if (c->s[i].sel == bx)
				x86_loadseg(c, i, bx);
		return 1;
	case 0x0009:
		LDT[SELIX(bx)].d_acc = (c->r[R_CX] & 0xff & ~D_DPL) | D_P;
		return 1;
	case 0x000a:
		s = sel_alias(bx, 0);
		SETW(R_AX, s);
		return 1;
	case 0x000b:		/* get the descriptor */
	case 0x000c:		/* set it */
		a = lin(c->s[S_ES].sel, c->r[R_DI] & 0xffff);
		if (!a || SELIX(bx) >= LDTSIZE) {
			carry(c, 1);
			return 1;
		}
		d = &LDT[SELIX(bx)];
		if (AX == 0x000b) {
			PW(a, d->d_limit);
			PW(a + 2, d->d_base);
			PB(a + 4, d->d_base >> 16);
			PB(a + 5, d->d_acc | 0x60);
			PB(a + 6, (d->d_limit >> 16) & 0xf);
			PB(a + 7, d->d_base >> 24);
		} else {
			d->d_limit = GW(a) | (M[a + 6] & 0xf) << 16;
			if (M[a + 6] & 0x80)
				d->d_limit = d->d_limit << 12 | 0xfff;
			d->d_base = GW(a) ? (GW(a + 2) | M[a + 4] << 16 | (u32)M[a + 7] << 24) :
			    (GW(a + 2) | M[a + 4] << 16 | (u32)M[a + 7] << 24);
			d->d_acc = (M[a + 5] & ~D_DPL) | D_P;
		}
		return 1;
	case 0x0100:		/* DOS memory */
		s = g_alloc(GMEM_ZEROINIT, (c->r[R_BX] & 0xffff) << 4, curtask ? curtask->t_hinst : 0);
		if (!s) {
			carry(c, 1);
			SETW(R_AX, 8);
			return 1;
		}
		SETW(R_AX, sel_base(s) >> 4);
		SETW(R_DX, s);
		return 1;
	case 0x0101:
		g_free(c->r[R_DX] & 0xffff);
		return 1;
	case 0x0200:
		SETW(R_CX, 0);
		SETW(R_DX, 0);
		return 1;
	case 0x0204:		/* the protected-mode vector, as set */
		SETW(R_CX, FPSEL(pmvec[c->r[R_BX] & 0xff]));
		c->r[R_DX] = FPOFF(pmvec[c->r[R_BX] & 0xff]);
		return 1;
	case 0x0205:
		pmvec[c->r[R_BX] & 0xff] = FP(c->r[R_CX] & 0xffff, c->r[R_DX] & 0xffff);
		return 1;
	case 0x0201:
		return 1;
	case 0x0400:
		SETW(R_AX, 0x005a);
		SETW(R_BX, 5);
		c->r[R_CX] = (c->r[R_CX] & ~0xff) | 3;
		SETW(R_DX, 0x0870);
		return 1;
	case 0x0500:		/* free memory information */
		a = lin(c->s[S_ES].sel, c->r[R_DI] & 0xffff);
		if (a) {
			memset(M + a, 0xff, 48);
			PL(a, g_free_bytes());
			PL(a + 4, g_free_bytes() / 4096);
			PL(a + 8, g_free_bytes() / 4096);
		}
		return 1;
	case 0x0600:
	case 0x0601:
	case 0x0702:
	case 0x0703:
		return 1;
	}
	w16_log("startwin: DPMI %04x not done\n", AX);
	carry(c, 1);
	return 1;
}


static int
intr(c, n)
	struct x86 *c;
	int n;
{
	/*
	 * A vector the program set (DOS 25h, DPMI 0205h) that we do not serve
	 * ourselves: as an interrupt into its handler.  WIN87EM's emulator
	 * takes INT 34h-3Eh so.
	 */
	if (pmvec[n] && n != 0x21 && n != 0x31 && n != 0x2f) {
		u32 sp;

		x86_push16(c, x86_flags(c));
		x86_push16(c, c->s[S_CS].sel);
		x86_push16(c, c->eip & 0xffff);
		x86_setflags(c, x86_flags(c) & ~0x0300);	/* IF, TF */
		if (x86_loadseg(c, S_CS, FPSEL(pmvec[n]))) {
			sp = c->r[R_SP];
			(void)sp;
			w16_fatal("INT %02Xh: its handler %04x:%04x is not there", n, FPSEL(pmvec[n]), FPOFF(pmvec[n]));
		}
		c->eip = FPOFF(pmvec[n]);
		return 1;
	}
	switch (n) {
	case 0x21:
		return dos_int21(c);
	case 0x31:
		return dpmi(c);
	case 0x2f:
		switch (AX) {
		case 0x1600:		/* not enhanced mode */
		case 0x1680:		/* release the time slice */
			c->r[R_AX] &= ~0xff;
			return 1;
		}
		return 1;
	case 0x1a:
		if ((AX >> 8) == 0) {
			u32 t = w16_ticks() * 182 / 10000;

			SETW(R_CX, t >> 16);
			SETW(R_DX, t);
			c->r[R_AX] &= ~0xff;
		}
		return 1;
	case 0x33:
		SETW(R_AX, 0);		/* no mouse driver: Windows has the mouse */
		return 1;
	case 0x10:
	case 0x16:
	case 0x5c:
	case 0x4b:
		return 1;
	case 0x03:
		return 1;
	case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
	case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e:
		w16_fatal("the program uses the floating-point emulator (INT %02Xh), and WIN87EM.DLL is not installed", n);
		return 0;
	}
	w16_log("startwin: INT %02Xh AX=%04x not done\n", n, AX);
	return 1;
}

/* W16_TRACE=MODULE:seg[:from-to] (seg 0: all): each instruction there, for debugging */
static char trmod[16];
static int trseg, trfrom, trto = 0xffff;

static void
trace(c)
	struct x86 *c;
{
	static struct module *m;
	int i, seg = 0;

	if (!m)
		m = mod_find(trmod);
	if (!m)
		return;
	for (i = 1; i <= m->m_nseg; i++)
		if (m->m_seg[i].ns_sel == c->s[S_CS].sel)
			seg = i;
	if (seg && (!trseg || seg == trseg) && (c->eip & 0xffff) >= trfrom && (c->eip & 0xffff) <= trto)
		w16_log("  %d:%04x AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x SP=%04x DS=%04x ES=%04x\n",
		    seg, c->eip & 0xffff, c->r[R_AX] & 0xffff, c->r[R_BX] & 0xffff, c->r[R_CX] & 0xffff,
		    c->r[R_DX] & 0xffff, c->r[R_SI] & 0xffff, c->r[R_DI] & 0xffff, c->r[R_BP] & 0xffff,
		    c->r[R_SP] & 0xffff, c->s[S_DS].sel, c->s[S_ES].sel);
}

static int
fault(c, n, err)
	struct x86 *c;
	int n, err;
{
	static char *names[] = { "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound",
		"Invalid opcode", "No coprocessor", "Double fault", "", "Invalid TSS", "Segment not present",
		"Stack fault", "General protection fault" };
	char msg[256];
	struct module *m = mod_byhandle(c->s[S_CS].sel);
	int seg = 0, i;

	if (m)
		for (i = 1; i <= m->m_nseg; i++)
			if (m->m_seg[i].ns_sel == c->s[S_CS].sel)
				seg = i;
	sprintf(msg, "%s in module %s at %04X:%04X.", n < 14 ? names[n] : "Exception",
	    m ? m->m_name : "<unknown>", seg, c->eip & 0xffff);
	w16_log("startwin: %s (selector %04x, error %04x)\n", msg, c->s[S_CS].sel, err);
	w16_log("  AX %04x BX %04x CX %04x DX %04x SI %04x DI %04x BP %04x SP %04x\n",
	    c->r[R_AX] & 0xffff, c->r[R_BX] & 0xffff, c->r[R_CX] & 0xffff, c->r[R_DX] & 0xffff,
	    c->r[R_SI] & 0xffff, c->r[R_DI] & 0xffff, c->r[R_BP] & 0xffff, c->r[R_SP] & 0xffff);
	w16_log("  DS %04x ES %04x SS %04x\n", c->s[S_DS].sel, c->s[S_ES].sel, c->s[S_SS].sel);
	if (desktop) {
		strcat(msg, "\n\nThe application will close.");
		user_messagebox(0, msg, "Application Error", MB_ICONHAND);
	}
	w16_exit(255);
	return 0;
}

/* ---- the drives ---- */

static void
mkdirs(root)
	char *root;
{
	char p[1100];

	mkdir(root, 0755);
	sprintf(p, "%s/windows", root);
	mkdir(p, 0755);
	sprintf(p, "%s/windows/system", root);
	mkdir(p, 0755);
	sprintf(p, "%s/temp", root);
	mkdir(p, 0755);
}

static void
usage()
{
	fprintf(stderr, "usage: startwin [-C dir] [-D X=dir]... [-m MB] [-g WxH] [-v] [-S script] [-o shot.ppm] [program [args]]\n"
	    "       startwin [-C dir] -install disk.img... | disks-directory | windows-directory\n");
	exit(2);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *home = getenv("HOME"), *cdir = 0, cmd[128], dos[300], prog[300], *p;
	int i, w = 640, h = 480, mb = 8, err, code, len;
	struct module *m;
	char cwd[1024], *inst = 0;

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
			cdir = argv[++i];
		else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
			p = argv[++i];
			if (!((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') || p[1] != '=')
				usage();
			drive_root[(p[0] | 0x20) - 'a'] = p + 2;
		} else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
			mb = atoi(argv[++i]);
		else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &w, &h) != 2)
				usage();
		} else if (strcmp(argv[i], "-v") == 0)
			w16_debug++;
		else if (strcmp(argv[i], "-vv") == 0)
			w16_debug += 2;
		else if (strcmp(argv[i], "-vvv") == 0)
			w16_debug += 3;
		else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc)
			scr_script = argv[++i];
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			scr_shot = argv[++i];
		else if (strcmp(argv[i], "-install") == 0)
			inst = argv[i];
		else if (strcmp(argv[i], "-strict") == 0)
			w16_strict = 1;
		else
			usage();
	}
	if (mb < 2 || mb > 64)
		usage();
	if (!cdir) {
		static char buf[1024];

		sprintf(buf, "%s/WIN16", home ? home : ".");
		cdir = buf;
	}
	mkdirs(cdir);
	if (inst) {
		extern int win_install();

		if (i >= argc)
			usage();
		return win_install(argc - i, argv + i, cdir);
	}
	if (!drive_root[2])
		drive_root[2] = cdir;
	if (home && !drive_root[7])
		drive_root[7] = home;
	if (!drive_root[17])
		drive_root[17] = "/";
	/* the current directory, as a drive if it is under one */
	dos_init();
	dos_drive = 2;
	if (getcwd(cwd, sizeof cwd) && home && strncmp(cwd, home, strlen(home)) == 0 && drive_root[7] == home) {
		extern int dos_chdir();
		char d[300];

		sprintf(d, "H:%s", cwd + strlen(home));
		for (p = d; *p; p++)
			if (*p == '/')
				*p = '\\';
		if (!d[2])
			strcpy(d + 2, "\\");
		dos_chdir(d);
	}
	mem_init((u32)mb << 20);
	x86_init(&thecpu, M, MSIZE, LDT);
	cpu = &thecpu;
	cpu->fpu = 1;		/* the x87 on the host's floating point (x87.c) */
	x87_init(cpu);
	cpu->prot = 1;
	cpu->cr0 = 1;
	cpu->intr = intr;
	cpu->fault = fault;
	cpu->thunk = thunk_dispatch;
	if (getenv("W16_TRACE") && sscanf(getenv("W16_TRACE"), "%15[^:]:%d:%x-%x", trmod, &trseg, &trfrom, &trto) >= 2)
		cpu->trace = trace;
	if (scr_open(w, h, 0) != 0) {
		fprintf(stderr, "startwin: no screen\n");
		return 1;
	}
	gdi_init();
	kernel_init();
	thunk_init();
	user_init();
	/* the program */
	if (i < argc)
		strncpy(prog, argv[i++], sizeof prog - 1);
	else
		sprintf(prog, "%s\\PROGMAN.EXE", windir);
	prog[sizeof prog - 1] = 0;
	for (p = prog; *p; p++)
		if (*p == '/')
			*p = '\\';
	cmd[0] = 0;
	for (len = 0; i < argc; i++) {
		if (len + strlen(argv[i]) + 2 > sizeof cmd)
			break;
		if (len)
			cmd[len++] = ' ';
		strcpy(cmd + len, argv[i]);
		len += strlen(argv[i]);
	}
	/* a host path to a file outside the drives: its directory becomes drive D: */
	if (dos_fullpath(prog, dos) != 0 || dos_hostpath(dos, cwd, sizeof cwd, 0) != 0) {
		/* try the host path as given */
		char host[1024], *slash;

		strcpy(host, prog);
		for (p = host; *p; p++)
			if (*p == '\\')
				*p = '/';
		if (access(host, 0) == 0) {
			static char dir[4096];

			if ((slash = strrchr(host, '/')) != 0) {
				*slash = 0;
				if (!realpath(host[0] ? host : "/", dir))
					strcpy(dir, host);
				p = slash + 1;
			} else {
				if (!getcwd(dir, sizeof dir))
					strcpy(dir, ".");
				p = host;
			}
			drive_root[3] = dir;
			sprintf(dos, "D:\\%s", p);
			w16_upper(dos);
		}
	}
	/* Program Manager with no groups yet: Setup's, by DDE, once it is up */
	if ((p = strrchr(dos, '\\')) != 0 && w16_stricmp(p + 1, "PROGMAN.EXE") == 0) {
		extern void ddesetup_arm();
		char ini[300], host[1024];

		sprintf(ini, "%s\\PROGMAN.INI", windir);
		if (dos_hostpath(ini, host, sizeof host, 0) != 0 || access(host, 0) != 0)
			ddesetup_arm();
	}
	m = mod_load(dos, &err);
	if (!m) {
		fprintf(stderr, "startwin: %s: %s\n", prog, err == 2 ? "not found" : err == 11 ? "not a Windows program" :
		    "cannot load");
		scr_close();
		return 1;
	}
	if (m->m_dll) {
		fprintf(stderr, "startwin: %s is a library, not a program\n", prog);
		scr_close();
		return 1;
	}
	{
		extern int dos_chdir();
		char dir[300];

		strcpy(dir, m->m_path);
		if ((p = strrchr(dir, '\\')) != 0) {
			if (p == dir + 2)
				p[1] = 0;
			else
				*p = 0;
			dos_chdir(dir);
		}
	}
	initdeps(m, 0);
	if (w16_debug)
		w16_log("startwin: %s (%s)\n", m->m_name, m->m_path);
	{
		struct task *t = task_new(m, cmd, SW_SHOWNORMAL);

		if (!t) {
			fprintf(stderr, "startwin: %s cannot start\n", prog);
			scr_close();
			return 1;
		}
		code = task_run(t);
	}
	scr_close();
	if (w16_debug)
		w16_log("startwin: %s ended (%d)\n", m->m_name, code);
	return code;
}
