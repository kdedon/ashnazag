/*
 * t_v200: the Voyage 200 adapter's image checks (SHA-256, CRC-32 test
 * vectors) and its tiplatform module: registered, loaded, opened.
 */
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sys/mod.h"
#include "hash.h"
#include "profile.h"
#include "t.h"

#define	MD	"/tests/aux/mod.d"

static int
hex(data, n, want)
	char *data, *want;
	unsigned long n;
{
	unsigned char digest[32];
	char out[65];
	int i;

	v_sha256((const unsigned char *)data, n, digest);
	for (i = 0; i < 32; i++)
		sprintf(out + 2 * i, "%02x", digest[i]);
	return strcmp(out, want) == 0;
}

static int
modid(name)
	char *name;
{
	struct modstatus st;
	int id = 1;

	while (modstat(id, &st, 1) == 0) {
		if (strcmp(st.ms_name, name) == 0)
			return st.ms_id;
		id = st.ms_id + 1;
	}
	return -1;
}

int
main()
{
	struct mod_mreg reg;
	int mj = TI_MAJOR, id, fd;

	t_init("v200", 60);
	t_check("sha256_abc", hex("abc", 3UL,
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "digest");
	t_check("sha256_empty", hex("", 0UL,
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "digest");
	t_check("crc32", v_crc((const unsigned char *)"123456789", 9UL) == 0xcbf43926UL, "crc");
	if (access(MD "/tiplatform", 0) < 0 || access("/dev/tiplatform", 0) < 0) {
		t_skip("module", "no tiplatform module on this root");
		return t_done();
	}
	modpath(MD);
	memset(&reg, 0, sizeof reg);
	strcpy(reg.md_modname, "tiplatform");
	reg.md_typedata = (caddr_t)&mj;
	if (!t_check("register_cdev", modadm(MOD_TY_CDEV, MOD_C_MREG, &reg) == 0 || errno == EEXIST,
	    "%s", T_ERR))
		return t_done();
	fd = open("/dev/tiplatform", O_RDWR);
	t_check("open_autoload", fd >= 0, "%s", T_ERR);
	if (fd >= 0)
		close(fd);
	id = modid("tiplatform");
	t_check("loaded", id > 0, "not in modstat");
	if (id > 0)
		t_check("unload", moduload(id) == 0, "%s", T_ERR);
	return t_done();
}
