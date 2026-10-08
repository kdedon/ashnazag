/*
 * fpu.c -- the x87: arithmetic, conversions and the library's functions
 * built with inline x87 code (-fpi87), each result a line of RESULT.TXT
 * against expect/fpu.txt.
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "result.h"

static volatile double one = 1.0, two = 2.0, three = 3.0, ten = 10.0, half = 0.5;

static void
line(char *what, double v)
{
	char buf[80];

	sprintf(buf, "%s %.12g", what, v);
	report(buf);
}

int PASCAL
WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
	char buf[80];
	volatile float f;
	volatile long l;
	volatile short s;

	if (!(GetWinFlags() & WF_80x87))
		report("no coprocessor");
	line("add", one + two);
	line("div", one / three);
	line("mul", three * ten * half);
	line("sub", one - ten);
	line("sqrt", sqrt(two));
	line("sin", sin(one));
	line("cos", cos(one));
	line("tan", tan(half));
	line("atan2", atan2(one, two));
	line("exp", exp(one));
	line("log", log(ten));
	line("log10", log10(three));
	line("pow", pow(two, half));
	line("floor", floor(-two - half));
	line("ceil", ceil(-two - half));
	line("fmod", fmod(ten, three));
	f = (float)(one / three);
	line("float", f);
	l = (long)(-three - 0.7);
	sprintf(buf, "trunc %ld", l);
	report(buf);
	s = (short)(ten * ten * 3.3);
	sprintf(buf, "short %d", s);
	report(buf);
	l = 1234567;
	line("fromlong", (double)l * ten);
	sprintf(buf, "less %d %d", one < two, two < one);
	report(buf);
	line("big", 1e15 * ten + one);
	return 0;
}
