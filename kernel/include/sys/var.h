/* Configured table sizes. */
#ifndef _SYS_VAR_H
#define _SYS_VAR_H

struct var {
	int	v_buf;
	int	v_call;
	int	v_proc;
	int	_v0;
	int	v_nglobpris;
	int	v_maxsyspri;
	int	v_clist;
	int	v_maxup;	/* processes per user */
	int	v_hbuf;
	int	v_hmask;
	int	v_pbuf;
	int	v_sptmap;
	int	v_maxpmem;
	int	v_autoup;
	int	v_bufhwm;
	int	_v1[2];
};
extern struct var v;

#endif
