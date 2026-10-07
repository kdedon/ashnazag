/*
 * amilib tunables.
 */

/* busy-wait loops per microsecond, when the kernel has no delayus():
 * about 25 on a 50 MHz 68060, 10 on a 25 MHz 68040 */
long	amilib_loops_us = 25;
