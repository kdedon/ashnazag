/* Compile-time check of the superblock offsets in macroot.h. */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/fs/s5param.h"
#include "sys/fs/s5filsys.h"
#undef getfs
#include "sys/fs/ufs_fs.h"
#include "macroot.h"

#define CHK(n, c)	char n[(c) ? 1 : -1]
#define OFF(t, f)	((int)&((t *)0)->f)

CHK(s5blk, SUPERBOFF == S5_SBBLK * 512);
CHK(s5off, OFF(struct filsys, s_magic) == S5_MAGOFF);
CHK(s5mag, FsMAGIC == S5_MAGIC);
CHK(ufsblk, SBLOCK == UFS_SBBLK);
CHK(ufsoff, OFF(struct fs, fs_magic) == UFS_MAGOFF);
CHK(ufsmag, FS_MAGIC == UFS_MAGIC);
