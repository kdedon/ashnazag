/*
 * Card 1 of the disk driver: SCSI disks, served by a loadable module
 * that sets ata_sdscsi while loaded.
 */
struct sdhook {
	char	*name;
	int	(*probe)();	/* (unit) 0 when a disk answers, else an errno */
	ulong	(*nblk)();	/* (unit) blocks */
	int	(*rdblk)();	/* (unit, blkno, buf) one block; 0 or an errno */
	void	(*queue)();	/* (cp) as sdqueue */
};

extern struct sdhook *volatile ata_sdscsi;
