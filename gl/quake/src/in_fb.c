/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// in_fb.c -- keyboard and mouse events of the display session
//
// /dev/kbd and /dev/mouse deliver real presses and releases, ADB or Amiga
// raw key codes, while the session vid_fb.c opened is in front.

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "quakedef.h"
#include "dsio.h"

extern int	vid_fbfd;

static int	kbd_fd = -1, mouse_fd = -1;
static byte	*keymap;
static float	mouse_x, mouse_y;

cvar_t	m_filter = {"m_filter", "0"};

static byte adbmap[128] = {
	'a', 's', 'd', 'f', 'h', 'g', 'z', 'x',		// 00
	'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
	'y', 't', '1', '2', '3', '4', '6', '5',		// 10
	'=', '9', '7', '-', '8', '0', ']', 'o',
	'u', '[', 'i', 'p', K_ENTER, 'l', 'j', '\'',	// 20
	'k', ';', '\\', ',', '/', 'n', 'm', '.',
	K_TAB, K_SPACE, '`', K_BACKSPACE, 0, K_ESCAPE, K_CTRL, K_ALT,	// 30
	K_SHIFT, 0, K_ALT, K_LEFTARROW, K_RIGHTARROW, K_DOWNARROW, K_UPARROW, 0,
	0, '.', 0, '*', 0, '+', 0, 0,			// 40
	0, 0, 0, '/', K_ENTER, 0, '-', 0,
	0, '=', '0', '1', '2', '3', '4', '5',		// 50
	'6', '7', 0, '8', '9', 0, 0, 0,
	K_F5, K_F6, K_F7, K_F3, K_F8, K_F9, 0, K_F11,	// 60
	0, 0, 0, 0, 0, K_F10, 0, K_F12,
	0, K_PAUSE, K_INS, K_HOME, K_PGUP, K_DEL, K_F4, K_END,	// 70
	K_F2, K_PGDN, K_F1, K_SHIFT, K_ALT, K_CTRL, 0, 0,
};

static byte amigamap[128] = {
	'`', '1', '2', '3', '4', '5', '6', '7',		// 00
	'8', '9', '0', '-', '=', '\\', 0, '0',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',		// 10
	'o', 'p', '[', ']', 0, '1', '2', '3',
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k',		// 20
	'l', ';', '\'', 0, 0, '4', '5', '6',
	0, 'z', 'x', 'c', 'v', 'b', 'n', 'm',		// 30
	',', '.', '/', 0, '.', '7', '8', '9',
	K_SPACE, K_BACKSPACE, K_TAB, K_ENTER, K_ENTER, K_ESCAPE, K_DEL, 0,	// 40
	0, 0, '-', 0, K_UPARROW, K_DOWNARROW, K_RIGHTARROW, K_LEFTARROW,
	K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8,	// 50
	K_F9, K_F10, 0, 0, '/', '*', '+', K_INS,
	K_SHIFT, K_SHIFT, 0, K_CTRL, K_ALT, K_ALT, K_PAUSE, K_PAUSE,	// 60
};

void IN_Init (void)
{
	struct evinfo	ei;

	Cvar_RegisterVariable (&m_filter);
	if (vid_fbfd < 0)
		return;
	kbd_fd = open ("/dev/kbd", O_RDONLY | O_NDELAY);
	if (kbd_fd >= 0 && (ioctl (kbd_fd, EVIOCBIND, vid_fbfd) < 0 ||
	    ioctl (kbd_fd, EVIOCGINFO, &ei) < 0))
	{
		close (kbd_fd);
		kbd_fd = -1;
	}
	if (kbd_fd < 0)
		Con_Printf ("IN_Init: no keyboard\n");
	else
		keymap = ei.ei_kset == EVK_AMIGA ? amigamap : adbmap;
	if (COM_CheckParm ("-nomouse"))
		return;
	mouse_fd = open ("/dev/mouse", O_RDONLY | O_NDELAY);
	if (mouse_fd >= 0 && ioctl (mouse_fd, EVIOCBIND, vid_fbfd) < 0)
	{
		close (mouse_fd);
		mouse_fd = -1;
	}
}

void IN_Shutdown (void)
{
	if (kbd_fd >= 0)
		close (kbd_fd);
	if (mouse_fd >= 0)
		close (mouse_fd);
	kbd_fd = mouse_fd = -1;
}

void Sys_SendKeyEvents (void)
{
	struct inev	ev[32];
	int		n, i, k;

	while (kbd_fd >= 0 && (n = read (kbd_fd, (char *)ev, sizeof(ev))) > 0)
		for (i = 0; i < n / (int)sizeof(ev[0]); i++)
			if (ev[i].ie_type == IE_KEY && ev[i].ie_code < 128 &&
			    (k = keymap[ev[i].ie_code]))
				Key_Event (k, ev[i].ie_value != 0);
	while (mouse_fd >= 0 && (n = read (mouse_fd, (char *)ev, sizeof(ev))) > 0)
		for (i = 0; i < n / (int)sizeof(ev[0]); i++)
			if (ev[i].ie_type == IE_BTN && ev[i].ie_code >= 1 && ev[i].ie_code <= 3)
				Key_Event (K_MOUSE1 + ev[i].ie_code - 1, ev[i].ie_value != 0);
			else if (ev[i].ie_type == IE_REL && ev[i].ie_code == IE_RELX)
				mouse_x += ev[i].ie_value;
			else if (ev[i].ie_type == IE_REL && ev[i].ie_code == IE_RELY)
				mouse_y += ev[i].ie_value;
}

void IN_Commands (void)
{
}

// id's svgalib mouse handling
void IN_Move (usercmd_t *cmd)
{
	static float	old_x, old_y;
	float		mx, my;

	if (mouse_fd < 0)
		return;
	if (m_filter.value)
	{
		mx = (mouse_x + old_x) * 0.5;
		my = (mouse_y + old_y) * 0.5;
	}
	else
	{
		mx = mouse_x;
		my = mouse_y;
	}
	old_x = mouse_x;
	old_y = mouse_y;
	mouse_x = mouse_y = 0;
	mx *= sensitivity.value;
	my *= sensitivity.value;

	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * mx;
	else
		cl.viewangles[YAW] -= m_yaw.value * mx;
	if (in_mlook.state & 1)
		V_StopPitchDrift ();
	if ((in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * my;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	}
	else if ((in_strafe.state & 1) && noclip_anglehack)
		cmd->upmove -= m_forward.value * my;
	else
		cmd->forwardmove -= m_forward.value * my;
}
