/* No holes known: every address goes to the stock driver. */
asm(".weak plat_kmemlen");
long
plat_kmemlen(a)
unsigned long a;
{
	return -1;
}
