/* Zero the 64 KB guest segment at p.  The target links segclra.s. */

int segclr(p)
char *p;
{
	register long i;

	for (i = 0; i < 0x10000L; i++)
		p[i] = 0;
	return (0);
}
