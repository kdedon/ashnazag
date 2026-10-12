/*
 * What Glide's DRI build takes from the PCI library and the x86 CPU
 * probe.  The kernel has the card set up, so configuration writes are
 * dropped; the probe reports a CPU with no extensions.
 */
#include "3dfx.h"
#include "fxpci.h"

const PciRegister PCI_COMMAND = { 0x4, 2, READ_WRITE };

FxBool
pciSetConfigData(PciRegister reg, FxU32 devNum, FxU32 *data)
{
	return FXTRUE;
}

FxI32
_cpu_detect_asm(void)
{
	return 0;
}
