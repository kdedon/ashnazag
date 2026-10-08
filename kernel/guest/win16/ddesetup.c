/*
 * ddesetup.c -- Program Manager's groups on its first run, as Windows
 * Setup made them: the groups and items SETUP.INF lists (its
 * [progman.groups] section and the sections it names), sent to Program
 * Manager as DDE commands (CreateGroup, AddItem, ShowGroup) once it is
 * up and idle.  Items whose program is not installed are left out, as
 * are the DOS prompt and Setup, which have no place here.  After them,
 * as Wabi's engine did, the commands of C:\WABI_GRP.TMP (one a line, its
 * group's: `-install' writes it from Wabi's wg_new.lst), which then goes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "w16.h"
#include "win.h"

#define	WM_DDE_INITIATE	0x03e0
#define	WM_DDE_TERMINATE 0x03e1
#define	WM_DDE_ACK	0x03e4
#define	WM_DDE_EXECUTE	0x03e8

#define	NCMD	32

static char *cmds[NCMD];	/* one execute a group */
static int ncmds, next, state;	/* 0 off, 1 looking for Program Manager, 2 executing, 3 waiting for the ack */
static struct wnd *me;
static u16 server, pending;
static int tries;
static int showmain;
static char grptmp[1024];	/* WABI_GRP.TMP, to go when done */

/* a host path for a DOS one, if the file is there */
static int
there(dir, name)
	char *dir, *name;
{
	char dos[300], host[1024];

	sprintf(dos, "%s\\%s", dir, name);
	return dos_hostpath(dos, host, sizeof host, 0) == 0 && access(host, 0) == 0;
}

static char *
section(t, name)
	char *t, *name;
{
	char head[64], *p;
	int n;

	sprintf(head, "[%.60s]", name);
	n = strlen(head);
	for (p = t; (p = strchr(p, '[')) != 0; p++)
		if (w16_strnicmp(p, head, n) == 0 && (p == t || p[-1] == '\n'))
			return p + n;
	return 0;
}

/* the next line of a section into buf: 0 at its end */
static char *
line(p, buf, n)
	char *p, *buf;
	int n;
{
	int i;

	while (*p == '\r' || *p == '\n')
		p++;
	if (!*p || *p == '[')
		return 0;
	for (i = 0; *p && *p != '\n' && *p != '\r'; p++)
		if (i < n - 1)
			buf[i++] = *p;
	buf[i] = 0;
	return p;
}

/* field k (0 up) of a comma-separated line, quotes and blanks stripped */
static void
field(l, k, out, n)
	char *l, *out;
	int k, n;
{
	int i = 0, q = 0;

	for (; *l && k > 0; l++) {
		if (*l == '"')
			q = !q;
		else if (*l == ',' && !q)
			k--;
	}
	while (*l == ' ' || *l == '\t')
		l++;
	for (; *l && (q || *l != ','); l++) {
		if (*l == '"') {
			q = !q;
			continue;
		}
		if (i < n - 1)
			out[i++] = *l;
	}
	while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
		i--;
	out[i] = 0;
}

/* SETUP.INF's groups into commands */
static void
readgroups(inf)
	char *inf;
{
	extern char windir[], sysdir[];
	char *p, *q, l[256], key[64], name[64], desc[80], exe[80], icon[80], idx[16], buf[2048];
	int len, grp;

	if (!(p = section(inf, "progman.groups")))
		return;
	while ((p = line(p, l, sizeof l)) != 0 && ncmds < NCMD) {
		if (l[0] == ';' || !(q = strchr(l, '=')))
			continue;
		*q = 0;
		field(l, 0, key, sizeof key);
		field(q + 1, 0, name, sizeof name);
		if (!name[0] || !(q = section(inf, key)))
			continue;
		len = sprintf(buf, "[CreateGroup(%s)]", name);
		for (grp = 0; (q = line(q, l, sizeof l)) != 0; ) {
			if (l[0] == ';')
				continue;
			field(l, 0, desc, sizeof desc);
			field(l, 1, exe, sizeof exe);
			field(l, 2, icon, sizeof icon);
			field(l, 3, idx, sizeof idx);
			if (!exe[0] || strstr(exe, ".PIF") || strstr(exe, ".pif") || !w16_stricmp(exe, "SETUP.EXE"))
				continue;
			if (!there(windir, exe) && !there(sysdir, exe))
				continue;
			if (len + 200 > (int)sizeof buf)
				break;
			if (icon[0])
				len += sprintf(buf + len, "[AddItem(%s,%s,%s,%s)]", exe, desc, icon, idx[0] ? idx : "0");
			else
				len += sprintf(buf + len, "[AddItem(%s,%s)]", exe, desc);
			grp++;
		}
		cmds[ncmds++] = strdup(buf);
	}
	showmain = section(inf, "group3") != 0;
}

static u32
proc(a)
	u32 *a;
{
	struct wnd *w = wnd_get(a[0]);

	if (a[1] == WM_DDE_ACK) {
		if (state == 1 && !server)
			server = LO16(a[2]);	/* sent back from the initiate */
		else if (state == 3) {
			if (w16_debug)
				w16_log("startwin: progman %s: %s\n", (a[3] & 0x8000) ? "done" : "failed", cmds[next - 1]);
			if (HI16(a[3]))
				g_free(HI16(a[3]));
			pending = 0;
			state = 2;
		}
		return 0;
	}
	return w ? user_defproc(w, a[1], a[2], a[3]) : 0;
}

/* Wabi's group: WABI_GRP.TMP's lines */
static void
readwabi()
{
	FILE *fp;
	char l[512], *p;

	if (dos_hostpath("C:\\WABI_GRP.TMP", grptmp, sizeof grptmp, 0) != 0 || (fp = fopen(grptmp, "r")) == 0) {
		grptmp[0] = 0;
		return;
	}
	while (fgets(l, sizeof l, fp) && ncmds < NCMD) {
		for (p = l + strlen(l); p > l && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); )
			*--p = 0;
		if (l[0] == '[')
			cmds[ncmds++] = strdup(l);
	}
	fclose(fp);
}

/* from startwin: Program Manager is starting, with no groups yet if fresh */
void
ddesetup_arm(fresh)
	int fresh;
{
	extern char sysdir[];
	char dos[300], host[1024], *inf;
	FILE *fp;
	long n;

	sprintf(dos, "%s\\SETUP.INF", sysdir);
	if (fresh && dos_hostpath(dos, host, sizeof host, 0) == 0 && (fp = fopen(host, "rb")) != 0) {
		fseek(fp, 0L, 2);
		n = ftell(fp);
		fseek(fp, 0L, 0);
		inf = (char *)malloc(n + 1);
		n = fread(inf, 1, n, fp);
		inf[n < 0 ? 0 : n] = 0;
		fclose(fp);
		readgroups(inf);
		free(inf);
	}
	readwabi();
	/* Main in front, as Setup leaves it */
	if (showmain && ncmds < NCMD)
		cmds[ncmds++] = strdup("[ShowGroup(Main,1)]");
	if (ncmds)
		state = 1;
}

/* when the program waits for input: the next step */
void
ddesetup_idle()
{
	static u32 cproc;
	struct wnd *t;
	u32 app, topic, h;
	char *s;

	if (!state)
		return;
	if (state == 1) {
		u32 sc;

		if (!me) {
			cproc = thunk_internal(proc, "wwwl", 'l', "DDESetupWndProc");
			cls_register("AshDDESetup", 0, cproc, 0, 0, 0, 0, 0, 0, (u32)0, 1);
			sc = ustr("AshDDESetup");
			me = wnd_create(0, sc, ustr(""), WS_POPUP, 0, 0, 0, 0, 0, 0, 0, 0);
			ufree(sc);
			if (!me) {
				state = 0;
				return;
			}
		}
		sc = ustr("PROGMAN");
		app = atom_add(sc);
		topic = atom_add(sc);
		ufree(sc);
		for (t = desktop->child; t && !server; t = t->next)
			if (t != me && (t->style & WS_VISIBLE))
				wnd_send(t, WM_DDE_INITIATE, me->h, FP(topic, app));
		if (server)
			state = 2;
		else if (++tries > 50)
			state = 0;
		return;
	}
	if (state == 2) {
		if (next >= ncmds || !wnd_get(server)) {
			if (wnd_get(server))
				wnd_post(server, WM_DDE_TERMINATE, me->h, 0);
			wnd_destroy(me);
			me = 0;
			state = 0;
			if (grptmp[0] && next >= ncmds)
				unlink(grptmp);
			return;
		}
		s = cmds[next++];
		h = g_alloc(GMEM_MOVEABLE | GMEM_DDESHARE | GMEM_ZEROINIT, strlen(s) + 1, 0);
		strcpy((char *)M + sel_base(h), s);
		pending = h;
		state = 3;
		wnd_post(server, WM_DDE_EXECUTE, me->h, FP(h, 0));
	}
}
