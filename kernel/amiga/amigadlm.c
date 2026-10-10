#include "sys/types.h"
#include "sys/mod.h"

extern int dlm_inited, dlm_creg(), dlm_xreg(), printf();

void
plat_dmainit()
{
	static char *names[] = { "uinter", "otbridge", "tosguest", "amigaguest" };
	struct mod_execreg er;
	int i, error;

	if (!dlm_inited)
		return;
	for (i = 0; i < 4; i++)
		if ((error = dlm_creg(names[i], 54 + i)) != 0)
			printf("dlm: %s major %d registration error %d\n", names[i], 54 + i, error);
	er.er_magic = 0x150;
	er.er_flags = EXF_FIRST;
	if ((error = dlm_xreg("auxexec", &er)) != 0)
		printf("dlm: auxexec registration error %d\n", error);
}
