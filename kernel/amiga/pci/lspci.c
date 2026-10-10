/*
 * lspci -- list the PCI devices behind the bridges.
 *
 *	lspci [-x]	-x: configuration space header in hex
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "sys/opci.h"
#include "sys/pciio.h"

extern int errno, ioctl();

static struct { int class; char *name; } classes[] = {
	{ 0x0100, "SCSI storage controller" },
	{ 0x0101, "IDE interface" },
	{ 0x0200, "Ethernet controller" },
	{ 0x0300, "VGA compatible controller" },
	{ 0x0380, "Display controller" },
	{ 0x0400, "Multimedia video controller" },
	{ 0x0401, "Multimedia audio controller" },
	{ 0x0600, "Host bridge" },
	{ 0x0604, "PCI bridge" },
	{ 0x0c03, "USB controller" },
	{ 0, 0 }
};

static char *bridges[] = {
	"Mediator 1200", "Mediator Z4", "Prometheus", "G-REX 1200", "G-REX 4000"
};

static char *
classname(c)
int c;
{
	int i;

	for (i = 0; classes[i].name; i++)
		if (classes[i].class == c)
			return classes[i].name;
	return "Unknown class";
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct pci_info pi;
	int fd, n, i, hex = argc > 1 && strcmp(argv[1], "-x") == 0;
	unsigned long id, cr;

	if ((fd = open("/dev/pci", O_RDONLY)) < 0) {
		perror("lspci: /dev/pci");
		return 1;
	}
	for (n = 0; ; n++) {
		pi.pi_index = n;
		if (ioctl(fd, PCIIOC_DEV, &pi) < 0)
			break;
		if (n == 0)
			for (i = 0; i < 5; i++)
				if (pi.pi_bridges & 1 << i)
					printf("bridge: %s\n", bridges[i]);
		id = pi.pi_cfg[0];
		cr = pi.pi_cfg[2];
		printf("%02x:%02x.%d %s [%04lx]: %04lx:%04lx (rev %02lx)\n",
		    pi.pi_bus, pi.pi_slot, pi.pi_func, classname((int)(cr >> 16)),
		    cr >> 16, id & 0xffff, id >> 16, cr & 0xff);
		if (hex)
			for (i = 0; i < 16; i++)
				printf("%s%02x: %08lx%s", i % 4 ? "" : "  ", i * 4,
				    pi.pi_cfg[i], i % 4 == 3 ? "\n" : " ");
	}
	if (errno != ENXIO) {
		perror("lspci");
		return 1;
	}
	return 0;
}
