/*
 * amixMacKeyMap.c -- core keymap for Apple Desktop Bus keyboards.
 *
 * X keycode = ADB key code + 8, as in the XKB "macintosh" keycodes.
 * Command is Meta (Mod1), Option is Mode_switch (Mod2).
 */

#include "amix.h"
#include "keysym.h"

#define MIN_ADB_KEY	8
#define MAX_ADB_KEY	(8 + 0x7F)
#define ADB_GLYPHS	2

static KeySym AdbMap[] = {
	XK_a,		NoSymbol,	/* 0x00 */
	XK_s,		NoSymbol,	/* 0x01 */
	XK_d,		NoSymbol,	/* 0x02 */
	XK_f,		NoSymbol,	/* 0x03 */
	XK_h,		NoSymbol,	/* 0x04 */
	XK_g,		NoSymbol,	/* 0x05 */
	XK_z,		NoSymbol,	/* 0x06 */
	XK_x,		NoSymbol,	/* 0x07 */
	XK_c,		NoSymbol,	/* 0x08 */
	XK_v,		NoSymbol,	/* 0x09 */
	XK_section,	XK_degree,	/* 0x0a ISO */
	XK_b,		NoSymbol,	/* 0x0b */
	XK_q,		NoSymbol,	/* 0x0c */
	XK_w,		NoSymbol,	/* 0x0d */
	XK_e,		NoSymbol,	/* 0x0e */
	XK_r,		NoSymbol,	/* 0x0f */

	XK_y,		NoSymbol,	/* 0x10 */
	XK_t,		NoSymbol,	/* 0x11 */
	XK_1,		XK_exclam,	/* 0x12 */
	XK_2,		XK_at,		/* 0x13 */
	XK_3,		XK_numbersign,	/* 0x14 */
	XK_4,		XK_dollar,	/* 0x15 */
	XK_6,		XK_asciicircum,	/* 0x16 */
	XK_5,		XK_percent,	/* 0x17 */
	XK_equal,	XK_plus,	/* 0x18 */
	XK_9,		XK_parenleft,	/* 0x19 */
	XK_7,		XK_ampersand,	/* 0x1a */
	XK_minus,	XK_underscore,	/* 0x1b */
	XK_8,		XK_asterisk,	/* 0x1c */
	XK_0,		XK_parenright,	/* 0x1d */
	XK_bracketright, XK_braceright,	/* 0x1e */
	XK_o,		NoSymbol,	/* 0x1f */

	XK_u,		NoSymbol,	/* 0x20 */
	XK_bracketleft,	XK_braceleft,	/* 0x21 */
	XK_i,		NoSymbol,	/* 0x22 */
	XK_p,		NoSymbol,	/* 0x23 */
	XK_Return,	NoSymbol,	/* 0x24 */
	XK_l,		NoSymbol,	/* 0x25 */
	XK_j,		NoSymbol,	/* 0x26 */
	XK_apostrophe,	XK_quotedbl,	/* 0x27 */
	XK_k,		NoSymbol,	/* 0x28 */
	XK_semicolon,	XK_colon,	/* 0x29 */
	XK_backslash,	XK_bar,		/* 0x2a */
	XK_comma,	XK_less,	/* 0x2b */
	XK_slash,	XK_question,	/* 0x2c */
	XK_n,		NoSymbol,	/* 0x2d */
	XK_m,		NoSymbol,	/* 0x2e */
	XK_period,	XK_greater,	/* 0x2f */

	XK_Tab,		NoSymbol,	/* 0x30 */
	XK_space,	NoSymbol,	/* 0x31 */
	XK_grave,	XK_asciitilde,	/* 0x32 */
	XK_BackSpace,	NoSymbol,	/* 0x33 Delete */
	XK_KP_Enter,	NoSymbol,	/* 0x34 PowerBook Enter */
	XK_Escape,	NoSymbol,	/* 0x35 */
	XK_Control_L,	NoSymbol,	/* 0x36 */
	XK_Meta_L,	NoSymbol,	/* 0x37 Command */
	XK_Shift_L,	NoSymbol,	/* 0x38 */
	XK_Caps_Lock,	NoSymbol,	/* 0x39 */
	XK_Mode_switch,	NoSymbol,	/* 0x3a Option */
	XK_Left,	NoSymbol,	/* 0x3b */
	XK_Right,	NoSymbol,	/* 0x3c */
	XK_Down,	NoSymbol,	/* 0x3d */
	XK_Up,		NoSymbol,	/* 0x3e */
	NoSymbol,	NoSymbol,	/* 0x3f */

	NoSymbol,	NoSymbol,	/* 0x40 */
	XK_KP_Decimal,	NoSymbol,	/* 0x41 */
	NoSymbol,	NoSymbol,	/* 0x42 */
	XK_KP_Multiply,	NoSymbol,	/* 0x43 */
	NoSymbol,	NoSymbol,	/* 0x44 */
	XK_KP_Add,	NoSymbol,	/* 0x45 */
	NoSymbol,	NoSymbol,	/* 0x46 */
	XK_Clear,	NoSymbol,	/* 0x47 */
	NoSymbol,	NoSymbol,	/* 0x48 */
	NoSymbol,	NoSymbol,	/* 0x49 */
	NoSymbol,	NoSymbol,	/* 0x4a */
	XK_KP_Divide,	NoSymbol,	/* 0x4b */
	XK_KP_Enter,	NoSymbol,	/* 0x4c */
	NoSymbol,	NoSymbol,	/* 0x4d */
	XK_KP_Subtract,	NoSymbol,	/* 0x4e */
	NoSymbol,	NoSymbol,	/* 0x4f */

	NoSymbol,	NoSymbol,	/* 0x50 */
	XK_KP_Equal,	NoSymbol,	/* 0x51 */
	XK_KP_0,	NoSymbol,	/* 0x52 */
	XK_KP_1,	NoSymbol,	/* 0x53 */
	XK_KP_2,	NoSymbol,	/* 0x54 */
	XK_KP_3,	NoSymbol,	/* 0x55 */
	XK_KP_4,	NoSymbol,	/* 0x56 */
	XK_KP_5,	NoSymbol,	/* 0x57 */
	XK_KP_6,	NoSymbol,	/* 0x58 */
	XK_KP_7,	NoSymbol,	/* 0x59 */
	NoSymbol,	NoSymbol,	/* 0x5a */
	XK_KP_8,	NoSymbol,	/* 0x5b */
	XK_KP_9,	NoSymbol,	/* 0x5c */
	NoSymbol,	NoSymbol,	/* 0x5d */
	NoSymbol,	NoSymbol,	/* 0x5e */
	NoSymbol,	NoSymbol,	/* 0x5f */

	XK_F5,		NoSymbol,	/* 0x60 */
	XK_F6,		NoSymbol,	/* 0x61 */
	XK_F7,		NoSymbol,	/* 0x62 */
	XK_F3,		NoSymbol,	/* 0x63 */
	XK_F8,		NoSymbol,	/* 0x64 */
	XK_F9,		NoSymbol,	/* 0x65 */
	NoSymbol,	NoSymbol,	/* 0x66 */
	XK_F11,		NoSymbol,	/* 0x67 */
	NoSymbol,	NoSymbol,	/* 0x68 */
	XK_F13,		NoSymbol,	/* 0x69 */
	NoSymbol,	NoSymbol,	/* 0x6a */
	XK_F14,		NoSymbol,	/* 0x6b */
	NoSymbol,	NoSymbol,	/* 0x6c */
	XK_F10,		NoSymbol,	/* 0x6d */
	NoSymbol,	NoSymbol,	/* 0x6e */
	XK_F12,		NoSymbol,	/* 0x6f */

	NoSymbol,	NoSymbol,	/* 0x70 */
	XK_F15,		NoSymbol,	/* 0x71 */
	XK_Help,	NoSymbol,	/* 0x72 Help/Insert */
	XK_Home,	NoSymbol,	/* 0x73 */
	XK_Prior,	NoSymbol,	/* 0x74 */
	XK_Delete,	NoSymbol,	/* 0x75 forward delete */
	XK_F4,		NoSymbol,	/* 0x76 */
	XK_End,		NoSymbol,	/* 0x77 */
	XK_F2,		NoSymbol,	/* 0x78 */
	XK_Next,	NoSymbol,	/* 0x79 */
	XK_F1,		NoSymbol,	/* 0x7a */
	XK_Shift_R,	NoSymbol,	/* 0x7b */
	XK_Mode_switch,	NoSymbol,	/* 0x7c right Option */
	XK_Control_R,	NoSymbol,	/* 0x7d */
	NoSymbol,	NoSymbol,	/* 0x7e */
	NoSymbol,	NoSymbol,	/* 0x7f Power */
};

#define	cT	(ControlMask)
#define	sH	(ShiftMask)
#define	lK	(LockMask)
#define	m1	(Mod1Mask)		/* Command */
#define	m2	(Mod2Mask)		/* Option */

/* indexed by X keycode */
CARD8 amixMacModMap[MAP_LENGTH] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 00-0f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 10-1f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 20-2f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, cT, m1, /* 30-3f */
   sH, lK, m2,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 40-4f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 50-5f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 60-6f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 70-7f */
    0,  0,  0, sH, m2, cT,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 80-8f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 90-9f */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* a0-af */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* b0-bf */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* c0-cf */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* d0-df */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* e0-ef */
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* f0-ff */
};

KeySymsRec amixMacKeySyms = {
    AdbMap, MIN_ADB_KEY, MAX_ADB_KEY, ADB_GLYPHS,
};

/*
 * German layout: letters and symbols of the German Atari keyboard at
 * their ADB positions.  Alternate becomes Mode_switch for @ \ [ ] { } ~.
 */
static struct { unsigned char code; KeySym k[4]; } deKeys[] = {
    { 0x06, { XK_y, NoSymbol } },
    { 0x10, { XK_z, NoSymbol } },
    { 0x13, { XK_2, XK_quotedbl } },
    { 0x14, { XK_3, XK_section } },
    { 0x16, { XK_6, XK_ampersand } },
    { 0x1A, { XK_7, XK_slash } },
    { 0x1C, { XK_8, XK_parenleft } },
    { 0x19, { XK_9, XK_parenright } },
    { 0x1D, { XK_0, XK_equal } },
    { 0x1B, { XK_ssharp, XK_question, XK_backslash } },
    { 0x18, { XK_apostrophe, XK_grave } },
    { 0x21, { XK_udiaeresis, XK_Udiaeresis, XK_at, XK_backslash } },
    { 0x1E, { XK_plus, XK_asterisk, XK_asciitilde } },
    { 0x29, { XK_odiaeresis, XK_Odiaeresis, XK_bracketleft, XK_braceleft } },
    { 0x27, { XK_adiaeresis, XK_Adiaeresis, XK_bracketright, XK_braceright } },
    { 0x32, { XK_numbersign, XK_asciicircum } },
    { 0x2A, { XK_asciitilde, XK_bar } },
    { 0x0A, { XK_less, XK_greater, XK_bar } },
    { 0x2B, { XK_comma, XK_semicolon } },
    { 0x2F, { XK_period, XK_colon } },
    { 0x2C, { XK_minus, XK_underscore } },
    { 0x37, { XK_Mode_switch, NoSymbol } },
};

static KeySym DeMap[0x80 * 4];

/* the keymap for layout name ("us", "de"); FALSE if unknown */
Bool
amixMacKeyLayout(name)
char *name;
{
    int i, j;

    if (strcmp(name, "us") == 0)
	return TRUE;
    if (strcmp(name, "de") != 0)
	return FALSE;
    for (i = 0; i < 0x80; i++)
	for (j = 0; j < 4; j++)
	    DeMap[i * 4 + j] = j < ADB_GLYPHS ? AdbMap[i * ADB_GLYPHS + j] : NoSymbol;
    for (i = 0; i < sizeof deKeys / sizeof deKeys[0]; i++)
	for (j = 0; j < 4; j++)
	    DeMap[deKeys[i].code * 4 + j] = deKeys[i].k[j];
    amixMacModMap[0x37 + 8] = m2;
    amixMacKeySyms.map = DeMap;
    amixMacKeySyms.mapWidth = 4;
    return TRUE;
}
