#include "sys/types.h"
#include "sys/mod.h"
extern int printf();
int dlm_inited;
static int n, major[4], bad;
static char *name[4];
int dlm_creg(s, m) char *s; int m; { name[n] = s; major[n++] = m; return 0; }
int dlm_xreg(s, er) char *s; struct mod_execreg *er;
{ if (s[0] != 'a' || er->er_magic != 0x150 || er->er_flags != EXF_FIRST) bad++; return 0; }
#include "../amigadlm.c"
int main()
{
	int i;
	plat_dmainit(); if (n) return 1;
	dlm_inited = 1; plat_dmainit();
	if (n != 4 || bad) return 1;
	for (i = 0; i < 4; i++) if (major[i] != 54 + i) return 1;
	if (name[0][0] != 'u' || name[1][0] != 'o' || name[2][0] != 't' || name[3][0] != 'a') return 1;
	printf("PASS startup disabled-loader guard and five registrations\n");
	return 0;
}
