/*
 * romscr.c -- lets one of fVDI's own Atari drivers start only on the
 * ROM's screen in the depths it draws (DEPTHS: a mask of 1 << planes,
 * as Line-A has them), so the driver list can name every driver in turn.
 */
#include "fvdi.h"
#include "driver.h"
#include "tosfb.h"

long CDECL fb_initialize(Virtual *vwk);

long CDECL initialize(Virtual *vwk)
{
	struct tfbcart *fc = (struct tfbcart *)TFB_CART;
	long d = vwk->real_address->screen.mfdb.bitplanes;

	if (*(long *)0xfa0000 != 0xabcdef42L || fc->fc_addr != 1 || d > 16 || !(DEPTHS >> d & 1))
		return 0;
	return fb_initialize(vwk);
}
