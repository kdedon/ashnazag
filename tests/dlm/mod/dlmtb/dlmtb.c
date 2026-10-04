/*
 * dlmtb -- MISC module depending on dlmta: its _load calls into dlmta
 * and reads a kernel variable.
 */

#include "sys/types.h"
#include "sys/moddefs.h"

extern int printf(), dlmta_add();
extern long lbolt;

int	dlmtb_result;
long	dlmtb_lbolt;

static int
dlmtb_load()
{
	dlmtb_result = dlmta_add(234);
	dlmtb_lbolt = lbolt;
	printf("dlmtb: loaded, dlmta_add(234) = %d\n", dlmtb_result);
	return 0;
}

MOD_MISC_WRAPPER(dlmtb, dlmtb_load, 0, "t_dlm dependent module");
