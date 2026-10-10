struct segdev_crargs { int (*mapfunc)(); off_t offset; dev_t dev; u_int prot, maxprot; };
struct segdev_data { off_t offset; u_int prot, maxprot; };
