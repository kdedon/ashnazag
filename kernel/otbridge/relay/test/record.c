#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../record.h"

static int checks, calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); exit(1); } checks++; } while (0)
static long answers[8];
static unsigned char received[100];
static unsigned long used;
static long
writer(void *arg, const unsigned char *bytes, unsigned long length)
{
	long n = answers[calls++];
	(void)arg;
	if (n > 0 && (unsigned long)n <= length) {
		memcpy(received + used, bytes, n); used += n;
	}
	return n;
}

int main(void)
{
	unsigned char b[100], ctl[4] = {0,0,0,OT_INFO_REQ}, data[3] = {1,2,3};
	struct otr_view v;
	struct otr_tx tx;
	int n;
	n = otr_pack(b, sizeof b, OTB_R_PROTO, ctl, 4, data, 3);
	CHECK(n == 15);
	CHECK(otr_view(b, n, &v) == OTR_DONE);
	CHECK(v.type == OTB_R_PROTO && !v.more && v.clen == 4 && v.dlen == 3);
	CHECK(!memcmp(v.ctl, ctl, 4) && !memcmp(v.data, data, 3));
	CHECK(otr_pack(b, 14, OTB_R_PROTO, ctl, 4, data, 3) == OTR_BAD);
	CHECK(otr_pack(b, 100, 2, ctl, 4, data, 3) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_DATA, ctl, 4, data, 3) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_PROTO, ctl, OTB_MAXCTL+1, data, 3) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_PROTO, ctl, 4, data, OTB_MAXDATA+1) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_PROTO, 0, 4, data, 3) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_PROTO, ctl, 3, data, 3) == OTR_BAD);
	CHECK(otr_view(b, 7, &v) == OTR_BAD);
	CHECK(otr_view(b, 14, &v) == OTR_BAD);
	CHECK(otr_view(b, 16, &v) == OTR_BAD);
	b[1] = 2; CHECK(otr_view(b, 15, &v) == OTR_BAD);
	b[1] = OTB_RF_MORE; CHECK(otr_view(b, 15, &v) == OTR_DONE && v.more);
	CHECK(otr_tx_start(&tx, b, 15) == OTR_BAD);
	b[1] = 0; CHECK(otr_tx_start(&tx, b, 15) == OTR_DONE);
	answers[0] = 3; answers[1] = 0; answers[2] = 5; answers[3] = 7;
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_WAIT && tx.sent == 3);
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_WAIT && tx.sent == 3);
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_WAIT && tx.sent == 8);
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_DONE && tx.sent == 15);
	CHECK(calls == 4 && used == 15 && !memcmp(received, b, 15));
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_DONE && calls == 4);
	CHECK(otr_tx_start(&tx, b, 15) == OTR_DONE);
	answers[4] = -1;
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_BAD && tx.sent == 0);
	answers[5] = 16;
	CHECK(otr_tx_step(&tx, writer, 0) == OTR_BAD && tx.sent == 0);
	CHECK(otr_tx_step(&tx, 0, 0) == OTR_BAD);
	CHECK(otr_pack(b, 100, OTB_R_DATA, 0, 0, data, 3) == 11);
	CHECK(otr_view(b, 11, &v) == OTR_DONE && v.clen == 0 && v.dlen == 3);
	b[4] = 0xff; CHECK(otr_view(b, 11, &v) == OTR_BAD);
	printf("PASS %d relay record checks\n", checks);
	return 0;
}
