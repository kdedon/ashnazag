/*
 * result.h -- the test programs' report: lines appended to RESULT.TXT
 * in the current directory, which the test script reads.
 */
#include <string.h>

static void
report(char *s)
{
	OFSTRUCT of;
	HFILE f;

	f = OpenFile("RESULT.TXT", &of, OF_READWRITE);
	if (f == HFILE_ERROR)
		f = _lcreat("RESULT.TXT", 0);
	if (f == HFILE_ERROR)
		return;
	_llseek(f, 0, 2);
	_lwrite(f, s, lstrlen(s));
	_lwrite(f, "\r\n", 2);
	_lclose(f);
}
