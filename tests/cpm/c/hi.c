#include "cpm.h"

int main(argc, argv)
int argc;
char **argv;
{
	cputs("HI ");
	cputs(argc > 1 ? argv[1] : "-");
	cputs("\r\n");
	return 0;
}
