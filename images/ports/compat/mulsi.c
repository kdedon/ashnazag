/* 32-bit multiply and divide for objects compiled at -m68000. */
int __mulsi3(int a, int b) { return a * b; }
int __divsi3(int a, int b) { return a / b; }
int __modsi3(int a, int b) { return a % b; }
unsigned __udivsi3(unsigned a, unsigned b) { return a / b; }
unsigned __umodsi3(unsigned a, unsigned b) { return a % b; }
