/*
 * /dev/pci: the PCI devices behind the host's bridges, for lspci.
 */

#ifndef _SYS_PCIIO_H
#define _SYS_PCIIO_H

#define	PCIIOC_DEV	(('P' << 8) | 1)	/* struct pci_info */

struct pci_info {
	int		pi_index;	/* in: device number, from 0 */
	int		pi_bridges;	/* OPCI_* bits */
	unsigned char	pi_bus, pi_slot, pi_func, pi_pad;
	unsigned long	pi_cfg[64];	/* configuration space */
};

#endif
