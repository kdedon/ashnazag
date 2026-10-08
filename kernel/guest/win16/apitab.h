/*
 * apitab.h -- the system modules' exports (apitab.c, made by mkapi.py).
 */
#ifndef APITAB_H
#define APITAB_H

struct apient {
	short	ae_ord;
	char	*ae_name;
	char	ae_kind;	/* p c v r s e V */
	char	*ae_args;	/* w s l p; equate: value; variable: dwords */
	char	ae_ret;		/* w or l */
};

struct apimod {
	char	*am_name;
	struct apient *am_ent;
};

extern struct apimod apimods[];

#endif
