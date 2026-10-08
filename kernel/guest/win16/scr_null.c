/*
 * scr_null.c -- the screen in memory only, for tests on any host: input
 * comes from a script, and the screen can be written as a PPM file.
 *
 * Script lines (# comments):
 *	wait MS			nothing for MS milliseconds
 *	move X Y		the pointer
 *	click X Y, rclick X Y, dclick X Y, down X Y, up X Y
 *	key VK [VK...]		a key pressed and let go (VK in hex or decimal)
 *	keydown VK, keyup VK
 *	type TEXT		the characters, with Shift as they need
 *	shot FILE		the screen to FILE now
 *	quit			ask the session to end
 * The end of the script is a quit.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win.h"
#include "scr.h"

struct surf screen;
int scr_mx, scr_my;
char *scr_shot, *scr_script;

static FILE *script;
static u32 waituntil;
static struct ev pend[64];
static int npend, ppos;
static int done;

int
scr_open(w, h, flags)
	int w, h, flags;
{
	screen.w = w;
	screen.h = h;
	screen.rowb = w;
	screen.mono = 0;
	screen.pix = (u8 *)calloc(1, w * h);
	if (scr_script && (script = fopen(scr_script, "r")) == 0) {
		perror(scr_script);
		return -1;
	}
	return 0;
}

void
scr_dirty(r)
	struct rect *r;
{
}

void
scr_setcursor(c)
	struct cursor *c;
{
}

void
scr_warp(x, y)
	int x, y;
{
	scr_mx = x;
	scr_my = y;
}

void
scr_beep()
{
}

void
scr_flush()
{
}

static int
writeppm(path)
	char *path;
{
	FILE *fp = fopen(path, "wb");
	int x, y;

	if (!fp) {
		perror(path);
		return -1;
	}
	fprintf(fp, "P6\n%d %d\n255\n", screen.w, screen.h);
	for (y = 0; y < screen.h; y++)
		for (x = 0; x < screen.w; x++)
			fwrite(syspal[screen.pix[y * screen.rowb + x]], 1, 3, fp);
	fclose(fp);
	return 0;
}

void
scr_close()
{
	if (scr_shot)
		writeppm(scr_shot);
}

static void
add(type, x, y, down, vk, btn)
	int type, x, y, down, vk, btn;
{
	struct ev *e;

	if (npend >= 64)
		return;
	e = &pend[npend++];
	memset(e, 0, sizeof *e);
	e->type = type;
	e->x = x;
	e->y = y;
	e->down = down;
	e->vk = vk;
	e->btn = btn;
}

static int
charvk(c, shift)
	int c, *shift;
{
	static char *sh = ")!@#$%^&*(";
	char *p;

	*shift = 0;
	if (c >= 'a' && c <= 'z')
		return c - 32;
	if (c >= 'A' && c <= 'Z') {
		*shift = 1;
		return c;
	}
	if (c >= '0' && c <= '9')
		return c;
	if ((p = strchr(sh, c)) != 0) {
		*shift = 1;
		return '0' + (p - sh);
	}
	switch (c) {
	case ' ': return VK_SPACE;
	case '\n': return VK_RETURN;
	case '\t': return VK_TAB;
	case '.': return 0xbe;
	case ',': return 0xbc;
	case '-': return 0xbd;
	case '=': return 0xbb;
	case '/': return 0xbf;
	case ';': return 0xba;
	case '\'': return 0xde;
	case ':': *shift = 1; return 0xba;
	case '?': *shift = 1; return 0xbf;
	case '_': *shift = 1; return 0xbd;
	case '+': *shift = 1; return 0xbb;
	case '"': *shift = 1; return 0xde;
	}
	return 0;
}

/* the next script line into events; 0 at the end */
static int
nextline()
{
	char line[512], cmd[32], arg[480];
	int x, y, vk, n, i, shift;

	for (;;) {
		if (!script || !fgets(line, sizeof line, script))
			return 0;
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || line[0] == 0)
			continue;
		arg[0] = 0;
		n = sscanf(line, "%31s %479[^\n]", cmd, arg);
		if (n < 1)
			continue;
		if (strcmp(cmd, "wait") == 0) {
			waituntil = w16_ticks() + atoi(arg);
			return 1;
		}
		if (strcmp(cmd, "move") == 0 && sscanf(arg, "%d %d", &x, &y) == 2) {
			add(EV_MOVE, x, y, 0, 0, 0);
			return 1;
		}
		if ((strcmp(cmd, "click") == 0 || strcmp(cmd, "rclick") == 0 || strcmp(cmd, "dclick") == 0 ||
		    strcmp(cmd, "down") == 0 || strcmp(cmd, "up") == 0) && sscanf(arg, "%d %d", &x, &y) == 2) {
			int b = cmd[0] == 'r';

			add(EV_MOVE, x, y, 0, 0, 0);
			if (cmd[0] != 'u')
				add(EV_BTN, x, y, 1, 0, b);
			if (cmd[0] != 'd' || cmd[1] == 'c')
				add(EV_BTN, x, y, 0, 0, b);
			if (cmd[0] == 'd' && cmd[1] == 'c') {
				add(EV_BTN, x, y, 1, 0, b);
				add(EV_BTN, x, y, 0, 0, b);
			}
			return 1;
		}
		/* clickid ID [dx dy]: on a control of the active window; clickitem ID N: on a list's item */
		if (strcmp(cmd, "clickid") == 0 || strcmp(cmd, "clickitem") == 0) {
			extern int ctl_scriptpoint();
			int id, a1 = 0, a2 = 0, n, item = strcmp(cmd, "clickitem") == 0;

			n = sscanf(arg, "%d %d %d", &id, &a1, &a2);
			if (n < 1 || (item && n < 2) ||
			    (item ? ctl_scriptpoint(id, a1, 0, 0, 0, &x, &y) :
			    ctl_scriptpoint(id, -1, a1, a2, n == 3, &x, &y)) != 0) {
				fprintf(stderr, "startwin: script: no control for \"%s\"\n", line);
				continue;
			}
			add(EV_MOVE, x, y, 0, 0, 0);
			add(EV_BTN, x, y, 1, 0, 0);
			add(EV_BTN, x, y, 0, 0, 0);
			return 1;
		}
		if (strcmp(cmd, "key") == 0 || strcmp(cmd, "keydown") == 0 || strcmp(cmd, "keyup") == 0) {
			char *p = arg, *e;

			while (*p) {
				vk = strtol(p, &e, 0);
				if (e == p)
					break;
				if (strcmp(cmd, "keyup") != 0)
					add(EV_KEY, 0, 0, 1, vk, 0);
				if (strcmp(cmd, "keydown") != 0)
					add(EV_KEY, 0, 0, 0, vk, 0);
				p = e;
				while (*p == ' ')
					p++;
			}
			return 1;
		}
		if (strcmp(cmd, "type") == 0) {
			for (i = 0; arg[i] && npend < 60; i++) {
				vk = charvk((u8)arg[i], &shift);
				if (!vk)
					continue;
				if (shift)
					add(EV_KEY, 0, 0, 1, VK_SHIFT, 0);
				add(EV_KEY, 0, 0, 1, vk, 0);
				add(EV_KEY, 0, 0, 0, vk, 0);
				if (shift)
					add(EV_KEY, 0, 0, 0, VK_SHIFT, 0);
			}
			return 1;
		}
		if (strcmp(cmd, "shot") == 0) {
			extern void user_flushpaint();

			user_flushpaint();
			writeppm(arg);
			continue;
		}
		if (strcmp(cmd, "tree") == 0) {
			extern void user_dumptree();

			user_dumptree();
			continue;
		}
		if (strcmp(cmd, "quit") == 0) {
			add(EV_QUIT, 0, 0, 0, 0, 0);
			return 1;
		}
		fprintf(stderr, "startwin: script: what is \"%s\"?\n", line);
	}
}

int
scr_poll(e, timeout)
	struct ev *e;
	int timeout;
{
	u32 start = w16_ticks();

	for (;;) {
		if (ppos < npend) {
			*e = pend[ppos++];
			if (ppos == npend)
				ppos = npend = 0;
			return 1;
		}
		if ((int)(w16_ticks() - waituntil) >= 0 && !done) {
			if (!nextline()) {
				done = 1;
				if (script) {
					add(EV_QUIT, 0, 0, 0, 0, 0);
					continue;
				}
			}
			continue;
		}
		if (timeout >= 0 && (int)(w16_ticks() - start) >= timeout)
			return 0;
		usleep(2000);
	}
}
