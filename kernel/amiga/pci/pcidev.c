/*
 * /dev/pci, major 60: configuration space of each device opci finds.
 * Read only.  ENXIO past the last device.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/errno.h"
#include "sys/conf.h"
#include "sys/systm.h"
#include "sys/moddefs.h"
#include "sys/opci.h"
#include "sys/pciio.h"

static int pci_flag;

static int
pciopen(devp, flag, otyp, cr)
dev_t *devp;
int flag, otyp;
char *cr;
{
	return 0;
}

static int
pciclose(dev, flag, otyp, cr)
dev_t dev;
int flag, otyp;
char *cr;
{
	return 0;
}

static int
pciioctl(dev, cmd, arg, mode, cr, rvalp)
dev_t dev;
int cmd, arg, mode;
char *cr;
int *rvalp;
{
	struct pci_info pi;
	struct opci_dev *d = 0;
	int i, n;

	if (cmd != PCIIOC_DEV)
		return EINVAL;
	if (copyin((caddr_t)arg, (caddr_t)&n, sizeof n))
		return EFAULT;
	for (i = 0; i <= n; i++)
		if (n < 0 || (d = opci_find(d, OPCI_ANY, OPCI_ANY)) == 0)
			return ENXIO;
	pi.pi_index = n;
	pi.pi_bridges = opci_bus();
	pi.pi_bus = opci_attr(d, OPCI_BUSNO);
	pi.pi_slot = opci_attr(d, OPCI_SLOT);
	pi.pi_func = opci_attr(d, OPCI_FUNCTION);
	for (i = 0; i < 64; i++)
		pi.pi_cfg[i] = opci_cfgread(d, i * 4, 4);
	return copyout((caddr_t)&pi, (caddr_t)arg, sizeof pi) ? EFAULT : 0;
}

struct mod_drv_data pci_drvdata[] = {
	{ { 0 }, 0, 0,
	  { pciopen, pciclose, 0, 0, pciioctl, 0, 0, 0, 0, 0, 0, 0, &pci_flag },
	  60, 1 }
};

static int
pci_load()
{
	return 0;
}

static int
pci_unload()
{
	return 0;
}

MOD_DRV_WRAPPER(pci, pci_load, pci_unload, 0, "PCI configuration space");
