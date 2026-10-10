/* cpmshim.c's services to the two interpreters' launchers */
#define	SEG	0x10000L
#define	NSEG	8		/* host segments one guest may hold */
extern char *shim_segget();
extern int shim_segput();
extern void shim_say(), shim_run80(), shim_run86(), shim_fini80(), shim_fini86();
extern long shim_read();
extern long cpm_shim();
extern void cpm_shimabort();
