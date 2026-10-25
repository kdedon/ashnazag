/*
 * The 32-bit multiply and divide calls of 68000 code, which this libgcc
 * lacks.  The rest is built for the 68000 so constant divisors call these:
 * at 68020 they become 64-bit multiplies, which the 68060 lacks.
 */
int __mulsi3(a, b) int a, b; { return a * b; }
int __divsi3(a, b) int a, b; { return a / b; }
int __modsi3(a, b) int a, b; { return a % b; }
unsigned __udivsi3(a, b) unsigned a, b; { return a / b; }
unsigned __umodsi3(a, b) unsigned a, b; { return a % b; }
