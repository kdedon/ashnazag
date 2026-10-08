/*
 * other.c -- the small system modules: KEYBOARD, SYSTEM, SOUND,
 * WIN87EM, MMSYSTEM, SHELL, VER and LZEXPAND, as far as programs
 * commonly use them.  Windows ANSI and the OEM code page are taken to
 * be one (ISO 8859-1 both ways).
 */

#include <stdlib.h>
#include <string.h>
#include "win.h"
#include "scr.h"

#define	STR(p)		(gptr(p) ? gptr(p) : "")

static u32 o_zero(a) u32 *a; { return 0; }
static u32 o_one(a) u32 *a; { return 1; }
static u32 o_minus(a) u32 *a; { return 0xffff; }

static u32
o_AnsiToOem(a)
	u32 *a;
{
	char *s = gptr(a[0]), *d = gptr(a[1]);

	if (s && d && s != d)
		memmove(d, s, strlen(s) + 1);
	return 1;
}

static u32
o_AnsiToOemBuff(a)
	u32 *a;
{
	char *s = gptr(a[0]), *d = gptr(a[1]);
	int n = a[2] ? a[2] : 65536;

	if (s && d && s != d)
		memmove(d, s, n);
	return 1;
}

static u32
o_VkKeyScan(a)
	u32 *a;
{
	int c = a[0] & 0xff;

	if (c >= 'a' && c <= 'z')
		return c - 32;
	if (c >= 'A' && c <= 'Z')
		return 0x100 | c;
	if (c >= '0' && c <= '9')
		return c;
	switch (c) {
	case ' ': return VK_SPACE;
	case '\r': return VK_RETURN;
	case '\t': return VK_TAB;
	case 8: return VK_BACK;
	case 27: return VK_ESCAPE;
	}
	return 0xffff;
}

static u32 o_GetKeyboardType(a) u32 *a; { return a[0] == 0 ? 4 : a[0] == 2 ? 12 : 0; }
static u32 o_MapVirtualKey(a) u32 *a; { return a[1] == 2 ? (a[0] >= 'A' && a[0] <= 'Z' ? a[0] : 0) : a[0]; }

static u32
o_GetKeyNameText(a)
	u32 *a;
{
	char *d = gptr(a[1]);

	if (d && (short)a[2] > 1)
		d[0] = 0;
	return 0;
}

static u32 o_GetSystemMsecCount(a) u32 *a; { return w16_ticks(); }
static u32 o_timeGetTime(a) u32 *a; { return w16_ticks(); }

/* WIN87EM's __fpMath: BX the function; nothing to set up without an x87 */
static u32
o_fpMath(a)
	u32 *a;
{
	cpu->r[R_AX] &= ~0xffff;
	return 0;
}

static u32
o_sndPlaySound(a)
	u32 *a;
{
	return 0;
}

static u32 o_MessageBeep(a) u32 *a; { scr_beep(); return 1; }

static u32
o_ShellExecute(a)
	u32 *a;
{
	w16_log("startwin: ShellExecute(\"%s\"): not done\n", STR(a[2]));
	return 31;		/* SE_ERR_NOASSOC */
}

static u32 o_RegError(a) u32 *a; { return 1; }	/* ERROR_BADDB */

static u32
o_LZOpenFile(a)
	u32 *a;
{
	extern u32 kernel_openfile();

	return kernel_openfile(a[0], a[1], a[2]);
}

static u32
o_LZCopy(a)
	u32 *a;
{
	char buf[4096];
	s32 n, t = 0;
	int s = a[0], d = a[1];
	u16 tmp = g_alloc(0, sizeof buf, 0);
	u32 l = sel_base(tmp);

	(void)buf;
	while ((n = dos_read(s, l, 4096)) > 0) {
		if (dos_write(d, l, n) != n) {
			t = -1;
			break;
		}
		t += n;
	}
	g_free(tmp);
	return t;
}

static u32 o_LZClose(a) u32 *a; { dos_close(a[0]); return 0; }
static u32 o_LZInit(a) u32 *a; { return a[0]; }

static u32
o_LZRead(a)
	u32 *a;
{
	u32 l = lin(FPSEL(a[1]), FPOFF(a[1]));
	s32 r = l ? dos_read(a[0], l, a[2] & 0xffff) : -1;

	return r < 0 ? 0xffff : r;
}

static u32
o_LZSeek(a)
	u32 *a;
{
	return dos_seek(a[0], (s32)a[1], a[2]);
}

struct impl o_impl[] = {
	{ "KEYBOARD", "AnsiToOem", o_AnsiToOem },
	{ "KEYBOARD", "OemToAnsi", o_AnsiToOem },
	{ "KEYBOARD", "AnsiToOemBuff", o_AnsiToOemBuff },
	{ "KEYBOARD", "OemToAnsiBuff", o_AnsiToOemBuff },
	{ "KEYBOARD", "VkKeyScan", o_VkKeyScan },
	{ "KEYBOARD", "GetKeyboardType", o_GetKeyboardType },
	{ "KEYBOARD", "MapVirtualKey", o_MapVirtualKey },
	{ "KEYBOARD", "GetKeyNameText", o_GetKeyNameText },
	{ "KEYBOARD", "OemKeyScan", o_minus },
	{ "KEYBOARD", "ToAscii", o_zero },
	{ "SYSTEM", "GetSystemMsecCount", o_GetSystemMsecCount },
	{ "SYSTEM", "InquireSystem", o_zero },
	{ "SOUND", "OpenSound", o_minus },
	{ "SOUND", "CloseSound", o_zero },
	{ "SOUND", "SetVoiceQueueSize", o_zero },
	{ "SOUND", "SetVoiceNote", o_zero },
	{ "SOUND", "SetVoiceAccent", o_zero },
	{ "SOUND", "StartSound", o_zero },
	{ "SOUND", "StopSound", o_zero },
	{ "SOUND", "WaitSoundState", o_zero },
	{ "SOUND", "SetVoiceSound", o_zero },
	{ "SOUND", "CountVoiceNotes", o_zero },
	{ "WIN87EM", "__fpMath", o_fpMath },
	{ "WIN87EM", "__WinEm87Info", o_zero },
	{ "WIN87EM", "__WinEm87Restore", o_zero },
	{ "WIN87EM", "__WinEm87Save", o_zero },
	{ "MMSYSTEM", "sndPlaySound", o_sndPlaySound },
	{ "MMSYSTEM", "MessageBeep", o_MessageBeep },
	{ "MMSYSTEM", "timeGetTime", o_timeGetTime },
	{ "MMSYSTEM", "waveOutGetNumDevs", o_zero },
	{ "MMSYSTEM", "midiOutGetNumDevs", o_zero },
	{ "MMSYSTEM", "auxGetNumDevs", o_zero },
	{ "MMSYSTEM", "mciSendCommand", o_minus },
	{ "MMSYSTEM", "mciSendString", o_minus },
	{ "SHELL", "ShellExecute", o_ShellExecute },
	{ "SHELL", "DragAcceptFiles", o_zero },
	{ "SHELL", "DragQueryFile", o_zero },
	{ "SHELL", "DragFinish", o_zero },
	{ "SHELL", "RegOpenKey", o_RegError },
	{ "SHELL", "RegCreateKey", o_RegError },
	{ "SHELL", "RegCloseKey", o_zero },
	{ "SHELL", "RegQueryValue", o_RegError },
	{ "SHELL", "RegSetValue", o_RegError },
	{ "SHELL", "RegEnumKey", o_RegError },
	{ "SHELL", "RegDeleteKey", o_RegError },
	{ "SHELL", "ExtractIcon", o_zero },
	{ "SHELL", "FindExecutable", o_zero },
	{ "VER", "GetFileVersionInfoSize", o_zero },
	{ "VER", "GetFileVersionInfo", o_zero },
	{ "VER", "VerQueryValue", o_zero },
	{ "LZEXPAND", "LZOpenFile", o_LZOpenFile },
	{ "LZEXPAND", "LZCopy", o_LZCopy },
	{ "LZEXPAND", "CopyLZFile", o_LZCopy },
	{ "LZEXPAND", "LZClose", o_LZClose },
	{ "LZEXPAND", "LZInit", o_LZInit },
	{ "LZEXPAND", "LZRead", o_LZRead },
	{ "LZEXPAND", "LZSeek", o_LZSeek },
	{ "LZEXPAND", "LZStart", o_one },
	{ "LZEXPAND", "LZDone", o_zero },
	{ 0 }
};
