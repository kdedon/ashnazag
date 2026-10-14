/*
 * Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * consw.c - Function 148: move to console 1, read a key there, come back.
 */

#include "cpm.h"

#define	BDOS_ATTACH	146
#define	BDOS_DETACH	147
#define	BDOS_SETCON	148
#define	BDOS_GETCON	153

int main()
{
	register int	r, c;

	cputs("CONSW: start\r\n");
	r = __bdos(BDOS_SETCON, 9L) & 0xff;
	cputs("CONSW: con9=");
	putdec((unsigned) r);
	cputs("\r\n");
	if ((__bdos(BDOS_SETCON, 1L) & 0xff) != 0) {
		cputs("CONSW: FAIL -- console 1 refused\r\n");
		return (1);
	}
	__bdos(BDOS_ATTACH, 0L);
	cputs("CONSW: on console ");
	putdec((unsigned) __bdos(BDOS_GETCON, 0L));
	cputs(", a key: ");
	c = __bdos(BDOS_CONIN, 0L) & 0xff;
	cputs("\r\n");
	__bdos(BDOS_DETACH, 0L);
	__bdos(BDOS_SETCON, 0L);
	cputs("CONSW: back on console ");
	putdec((unsigned) __bdos(BDOS_GETCON, 0L));
	cputs(", got ");
	conout(c);
	cputs("\r\n");
	return (0);
}
