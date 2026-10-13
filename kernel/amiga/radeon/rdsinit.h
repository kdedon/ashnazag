/* rdi_check and rdi_init results */
#define	RDI_NOROM	1	/* no 55 AA / PCIR image */
#define	RDI_NOTX86	2	/* Open Firmware (Mac) image */
#define	RDI_NOTABLES	3	/* no legacy table header */
#define	RDI_ATOM	4	/* ATOM BIOS: R300 and later */

extern int rdi_check();		/* (rom, len) */
extern int rdi_init();		/* (rom, len, r100) */
