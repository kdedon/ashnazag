/*
 * dlmth -- takes kernel memory until none is left, then gives 3.5 MB
 * back: enough to load a module, too little for dlmtk's 4 MB.
 * Unloading frees the rest.
 */

#include "sys/types.h"
#include "sys/kmem.h"
#include "sys/moddefs.h"

#define	PIECE	(256 * 1024)
#define	NPIECE	512
#define	NBACK	14

static caddr_t dlmth_p[NPIECE];
int	dlmth_n;

static int
dlmth_load()
{
	int k;

	while (dlmth_n < NPIECE &&
	    (dlmth_p[dlmth_n] = kmem_alloc(PIECE, KM_NOSLEEP)) != 0)
		dlmth_n++;
	for (k = 0; k < NBACK && dlmth_n > 0; k++)
		kmem_free(dlmth_p[--dlmth_n], PIECE);
	return 0;
}

static int
dlmth_unload()
{
	while (dlmth_n > 0)
		kmem_free(dlmth_p[--dlmth_n], PIECE);
	return 0;
}

MOD_MISC_WRAPPER(dlmth, dlmth_load, dlmth_unload, "memory hog");
