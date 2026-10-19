/* built without -fPIC: libdlb.so's text then needs relocating at load */
extern int dlb_data;

int
dlb_abs(void)
{
	return dlb_data + 1;
}
