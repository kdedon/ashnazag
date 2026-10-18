/* Tunable paging limits. */
#ifndef _SYS_TUNEABLE_H
#define _SYS_TUNEABLE_H

typedef struct tune {
	int	t_gpgslo;
	int	_t0[7];
	int	t_fsflushr;
	int	t_minarmem;	/* resident pages kept free */
	int	t_minasmem;
	int	t_flckrec;
} tune_t;
extern tune_t	tune;

#endif
