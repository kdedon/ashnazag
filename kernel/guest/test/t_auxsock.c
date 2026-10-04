/*
 * t_auxsock.c -- host checks of the A/UX socket numbering.
 */
#include <stdio.h>

extern int aux_socktype_in(), aux_socktype_out();
extern unsigned long aux_sockioc();

static int bad;

static void
check(name, ok)
	char *name;
	int ok;
{
	printf("%s %s\n", ok ? "OK  " : "FAIL", name);
	bad |= !ok;
}

int
main()
{
	int t, rt = 1;

	check("stream_swapped", aux_socktype_in(1L) == 2 && aux_socktype_out(2L) == 1);
	check("dgram_swapped", aux_socktype_in(2L) == 1 && aux_socktype_out(1L) == 2);
	check("raw", aux_socktype_in(3L) == 4);
	for (t = 1; t <= 5; t++)
		rt &= aux_socktype_out((long)aux_socktype_in((long)t)) == t;
	check("types_round_trip", rt);
	check("type_bad", aux_socktype_in(0L) == -1 && aux_socktype_in(6L) == -1 &&
	    aux_socktype_out(3L) == -1);
	check("netmask_renumbered", aux_sockioc(0xc0206917UL) == 0xc0206919UL);
	check("brdaddr_renumbered", aux_sockioc(0xc020691bUL) == 0xc0206917UL);
	check("ifconf_flags_same", aux_sockioc(0xc0086914UL) == 0xc0086914UL &&
	    aux_sockioc(0xc0206911UL) == 0xc0206911UL);
	check("async_nbio_nread_pgrp", aux_sockioc(0x8004667dUL) && aux_sockioc(0x8004667eUL) &&
	    aux_sockioc(0x4004667fUL) && aux_sockioc(0x80047308UL) == 0x80047308UL);
	check("tty_not_socket", aux_sockioc(0x40125401UL) == 0);
	return bad;
}
