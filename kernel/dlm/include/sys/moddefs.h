/*
 * Module wrappers and linkages, for module writers and the kernel.
 */

#ifndef _SYS_MODDEFS_H
#define _SYS_MODDEFS_H

#define	MODREV	1

struct mod_operations {
	int	(*modm_install)();	/* (struct dlm_mod *, void *typedata) */
	int	(*modm_remove)();	/* (struct dlm_mod *, void *typedata) */
	void	(*modm_info)();		/* (struct dlm_mod *, void *typedata,
					 *  struct modspecific_stat *) */
};

struct modlink {
	struct mod_operations	*ml_ops;
	void			*ml_type_data;
};

struct mod_type_data {
	char	*mtd_desc;
	void	*mtd_pdata;
};

struct mod_conf_data {
	int	mcd_unload_delay;	/* seconds */
};

struct modwrapper {
	int			mw_rev;
	int			(*mw_load)();
	int			(*mw_unload)();
	void			(*mw_halt)();
	struct mod_conf_data	*mw_conf;
	struct modlink		*mw_modlink;	/* ends with { 0, 0 } */
};

extern struct mod_operations mod_miscops;
extern struct mod_operations mod_execops;
extern struct mod_operations mod_hookops;

extern int mod_hold();			/* (struct modwrapper *) */
extern void mod_rele();			/* (struct modwrapper *) */

/* exec formats: array, ends with ex_func == 0 */
struct mod_exec_data {
	short	ex_magic;
	short	ex_flags;
	int	(*ex_func)();		/* as an execsw exec_func */
	int	(*ex_core)();		/* unused */
};

/* hooks: array, ends with hd_name == 0 */
struct mod_hook_data {
	char	*hd_name;
	int	(*hd_fn)();
};

/*
 * A named pointer a static shim calls through.  The static kernel
 * defines hooksw[], ending with hk_name == 0.
 */
struct hooksw {
	char	*hk_name;
	char	**hk_ptr;
	char	*hk_dflt;
	char	*hk_owner;		/* loaded module, 0 = free */
};

/*
 * Each wrapper macro defines <prefix>_wrapper and one linkage.  The
 * generated <prefix>_conf_data is referenced weakly, so a module
 * built without it still links; the loader then uses the default
 * unload delay.
 */
#ifdef __STDC__
#define	MOD_WRAPPER_(p, ops, load, unload, halt, desc) \
	asm(".weak " #p "_conf_data"); \
	extern struct mod_conf_data p##_conf_data; \
	static struct mod_type_data p##_mod_td = { desc, 0 }; \
	static struct modlink p##_mod_link[] = { \
		{ &ops, (void *)&p##_mod_td }, { 0, 0 } }; \
	struct modwrapper p##_wrapper = { \
		MODREV, load, unload, halt, &p##_conf_data, p##_mod_link }
#else
#define	MOD_WRAPPER_(p, ops, load, unload, halt, desc) \
	asm(".weak p" "_conf_data"); \
	extern struct mod_conf_data p/**/_conf_data; \
	static struct mod_type_data p/**/_mod_td = { desc, 0 }; \
	static struct modlink p/**/_mod_link[] = { \
		{ &ops, (char *)&p/**/_mod_td }, { 0, 0 } }; \
	struct modwrapper p/**/_wrapper = { \
		MODREV, load, unload, halt, &p/**/_conf_data, p/**/_mod_link }
#endif

#define	MOD_MISC_WRAPPER(p, load, unload, desc) \
	MOD_WRAPPER_(p, mod_miscops, load, unload, 0, desc)

/*
 * One linkage whose type data points at the module's own
 * <prefix>_execdata / <prefix>_hookdata array.
 */
#ifdef __STDC__
#define	MOD_TWRAPPER_(p, ops, sfx, load, unload, desc) \
	extern struct mod_##sfx##_data p##_##sfx##data[]; \
	static struct mod_type_data p##_mod_td = \
		{ desc, (void *)p##_##sfx##data }; \
	static struct modlink p##_mod_link[] = { \
		{ &ops, (void *)&p##_mod_td }, { 0, 0 } }; \
	asm(".weak " #p "_conf_data"); \
	extern struct mod_conf_data p##_conf_data; \
	struct modwrapper p##_wrapper = { \
		MODREV, load, unload, 0, &p##_conf_data, p##_mod_link }
#define	MOD_EXEC_WRAPPER(p, load, unload, desc) \
	MOD_TWRAPPER_(p, mod_execops, exec, load, unload, desc)
#define	MOD_HOOK_WRAPPER(p, load, unload, desc) \
	MOD_TWRAPPER_(p, mod_hookops, hook, load, unload, desc)
#else
#define	MOD_EXEC_WRAPPER(p, load, unload, desc) \
	extern struct mod_exec_data p/**/_execdata[]; \
	static struct mod_type_data p/**/_mod_td = \
		{ desc, (char *)p/**/_execdata }; \
	static struct modlink p/**/_mod_link[] = { \
		{ &mod_execops, (char *)&p/**/_mod_td }, { 0, 0 } }; \
	asm(".weak p" "_conf_data"); \
	extern struct mod_conf_data p/**/_conf_data; \
	struct modwrapper p/**/_wrapper = { \
		MODREV, load, unload, 0, &p/**/_conf_data, p/**/_mod_link }
#define	MOD_HOOK_WRAPPER(p, load, unload, desc) \
	extern struct mod_hook_data p/**/_hookdata[]; \
	static struct mod_type_data p/**/_mod_td = \
		{ desc, (char *)p/**/_hookdata }; \
	static struct modlink p/**/_mod_link[] = { \
		{ &mod_hookops, (char *)&p/**/_mod_td }, { 0, 0 } }; \
	asm(".weak p" "_conf_data"); \
	extern struct mod_conf_data p/**/_conf_data; \
	struct modwrapper p/**/_wrapper = { \
		MODREV, load, unload, 0, &p/**/_conf_data, p/**/_mod_link }
#endif

/* host bus adapter: no slot, no auto-load; _unload returns EBUSY */
#define	MOD_HDRV_WRAPPER(p, load, unload, halt, desc) \
	MOD_WRAPPER_(p, mod_miscops, load, unload, halt, desc)

#endif	/* _SYS_MODDEFS_H */
