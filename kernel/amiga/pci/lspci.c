/*
 * lspci -- list the PCI devices behind the bridges.
 *
 *	lspci [-x]	-x: configuration space header in hex
 *
 * Vendor and device names come from IDS, in the pci.ids format: a
 * vendor line "vvvv  name", its devices "\tdddd  name".  Without the
 * file or an entry, the numbers.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "sys/opci.h"
#include "sys/pciio.h"

#ifndef IDS
#define	IDS	"/usr/share/lib/pci.ids"
#endif
#define	NDEV	64
#define	NAME	80

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
	"Elbox Mediator 1200", "Elbox Mediator Z4", "Matay Prometheus",
	"DCE G-REX 1200", "DCE G-REX 4000"
};

static struct pci_info dev[NDEV];
static char vname[NDEV][NAME], dname[NDEV][NAME];

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

static int
hex4(s, v)
char *s;
unsigned long *v;
{
	int i, c;

	*v = 0;
	for (i = 0; i < 4; i++) {
		c = s[i];
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'a' && c <= 'f')
			c -= 'a' - 10;
		else if (c >= 'A' && c <= 'F')
			c -= 'A' - 10;
		else
			return 0;
		*v = *v << 4 | c;
	}
	return s[4] == ' ';
}

static void
setname(d, s)
char *d, *s;
{
	int n;

	while (*s == ' ')
		s++;
	strncpy(d, s, NAME - 1);
	d[NAME - 1] = 0;
	n = strlen(d);
	while (n > 0 && (d[n - 1] == '\n' || d[n - 1] == ' '))
		d[--n] = 0;
}

/* one pass over the IDS file for all n devices */
static void
names(n)
int n;
{
	FILE *f;
	char line[256];
	unsigned long v, ven = 0xffffffff;
	int i, whole = 1, start;

	if ((f = fopen(IDS, "r")) == 0)
		return;
	while (fgets(line, sizeof line, f)) {
		start = whole;
		whole = strchr(line, '\n') != 0;
		if (!start || line[0] == '#')
			continue;
		if (line[0] == 'C' && line[1] == ' ')
			break;		/* the class lists follow the vendors */
		if (line[0] != '\t') {
			ven = hex4(line, &v) ? v : 0xffffffff;
			for (i = 0; i < n; i++)
				if ((dev[i].pi_cfg[0] & 0xffff) == ven)
					setname(vname[i], line + 5);
		} else if (line[1] != '\t' && hex4(line + 1, &v))
			for (i = 0; i < n; i++)
				if ((dev[i].pi_cfg[0] & 0xffff) == ven &&
				    dev[i].pi_cfg[0] >> 16 == v)
					setname(dname[i], line + 6);
	}
	fclose(f);
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct pci_info *pi;
	int fd, n, k, i, hex = argc > 1 && strcmp(argv[1], "-x") == 0;
	unsigned long id, cr;

	if ((fd = open("/dev/pci", O_RDONLY)) < 0) {
		perror("lspci: /dev/pci");
		return 1;
	}
	for (n = k = 0; k < NDEV; n++) {
		dev[k].pi_index = n;
		if (ioctl(fd, PCIIOC_DEV, &dev[k]) < 0)
			break;
		if (n == 0)
			for (i = 0; i < 5; i++)
				if (dev[k].pi_bridges & 1 << i)
					printf("bridge: %s\n", bridges[i]);
		if ((dev[k].pi_cfg[0] & 0xffff) != 0xffff)	/* empty slot */
			k++;
	}
	if (k < NDEV && errno != ENXIO) {
		perror("lspci");
		return 1;
	}
	names(k);
	for (pi = dev; pi < dev + k; pi++) {
		id = pi->pi_cfg[0];
		cr = pi->pi_cfg[2];
		printf("%02x:%02x.%d %s [%04lx]: ", pi->pi_bus, pi->pi_slot,
		    pi->pi_func, classname((int)(cr >> 16)), cr >> 16);
		i = pi - dev;
		if (vname[i][0])
			printf("%s %s ", vname[i], dname[i][0] ? dname[i] : "Device");
		printf("[%04lx:%04lx] (rev %02lx)\n", id & 0xffff, id >> 16, cr & 0xff);
		if (hex)
			for (i = 0; i < 16; i++)
				printf("%s%02x: %08lx%s", i % 4 ? "" : "  ", i * 4,
				    pi->pi_cfg[i], i % 4 == 3 ? "\n" : " ");
	}
	return 0;
}
