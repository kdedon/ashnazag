/*
 * fbtty.c -- keyboard input for the console tty.
 *
 * The console tty is the SCC channel A stream (major 0, minor 0); its
 * output is mirrored on the screen by scc_txfill.  Bytes from the
 * keyboard driver, and the terminal's own answers, enter its receive
 * ring as if they had arrived on the serial line.
 */
extern void scc_conin();

void
fbcons_input(c)
int c;
{
	scc_conin(c & 0xFF);
}
