/* libkstat: snapshots of /dev/kstat, records looked up by name. */
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "kstat.h"

#define KS_MAX	128

kstat_ctl_t *
kstat_open()
{
	kstat_ctl_t *kc;

	if ((kc = (kstat_ctl_t *)calloc(1, sizeof *kc)) == 0)
		return 0;
	kc->kc_max = KS_MAX;
	kc->kc_data = (kstat_named_t *)malloc(sizeof kc->kc_hdr + KS_MAX * sizeof(kstat_named_t));
	if (kc->kc_data == 0 || (kc->kc_fd = open("/dev/kstat", O_RDONLY)) < 0) {
		free(kc->kc_data);
		free(kc);
		return 0;
	}
	fcntl(kc->kc_fd, F_SETFD, 1);
	if (kstat_read(kc) < 0) {
		kstat_close(kc);
		return 0;
	}
	return kc;
}

int
kstat_read(kc)
	kstat_ctl_t *kc;
{
	char *b = (char *)kc->kc_data;
	int n;

	n = read(kc->kc_fd, b, sizeof kc->kc_hdr + kc->kc_max * sizeof(kstat_named_t));
	if (n < (int)sizeof kc->kc_hdr)
		return -1;
	memcpy((char *)&kc->kc_hdr, b, sizeof kc->kc_hdr);
	if (kc->kc_hdr.ks_magic != KSTAT_MAGIC)
		return -1;
	/* no divide: a constant divisor compiles to a 64-bit multiply */
	n -= sizeof kc->kc_hdr;
	while (kc->kc_hdr.ks_ndata * sizeof(kstat_named_t) > n)
		kc->kc_hdr.ks_ndata--;
	memmove(b, b + sizeof kc->kc_hdr, kc->kc_hdr.ks_ndata * sizeof(kstat_named_t));
	return 0;
}

kstat_named_t *
kstat_data_lookup(kc, name)
	kstat_ctl_t *kc;
	char *name;
{
	unsigned long i;

	for (i = 0; i < kc->kc_hdr.ks_ndata; i++)
		if (strncmp(kc->kc_data[i].name, name, KSTAT_STRLEN) == 0)
			return &kc->kc_data[i];
	return 0;
}

unsigned long
kstat_value(kc, name)
	kstat_ctl_t *kc;
	char *name;
{
	kstat_named_t *k = kstat_data_lookup(kc, name);

	return k ? k->value : 0;
}

int
kstat_close(kc)
	kstat_ctl_t *kc;
{
	int r = close(kc->kc_fd);

	free(kc->kc_data);
	free(kc);
	return r;
}
