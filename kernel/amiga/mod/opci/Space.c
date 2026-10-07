/*
 * opci tunables.
 */

/*
 * openpci.library (the archive's Libs/openpci.library), its optional
 * PCI-Configuration (ENVARC:) and plugins (LIBS:PCI/) all live here.
 * Every AmigaOS name the library uses means a file in this directory.
 */
char	opci_confdir[] = "/etc/conf/pci";

/* busy-wait loops per microsecond, when the kernel has no delayus():
 * about 25 on a 50 MHz 68060, 10 on a 25 MHz 68040 */
long	opci_loops_us = 25;
