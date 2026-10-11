#ifndef MIG_AMFM_H
#define MIG_AMFM_H
/* AmigaDOS tracks in MFM: 1088 bytes a sector, a gap in front */
#define AMFM_SECTOR 1088
#define AMFM_GAP 1660
#define AMFM_WLEN(nsec) (AMFM_GAP + (nsec) * AMFM_SECTOR + 2)
/* enough for every sector once, read from anywhere in the revolution */
#define AMFM_RLEN(nsec) ((nsec) * AMFM_SECTOR + 2 * AMFM_GAP + AMFM_SECTOR)
/* one revolution, as the drive turns: the gap the next write leaves, then the sectors */
#define AMFM_REV(nsec) ((nsec) == 22 ? 25336L : 12668L)

/* the decoders' errors, trackdisk's numbers */
#define AMFM_NOSECHDR 21
#define AMFM_BADHDRSUM 24
#define AMFM_BADSECSUM 25
#define AMFM_TOOFEWSECS 26

int amfm_decode(const unsigned char *, long, int, int, unsigned char *);
long amfm_encode(int, int, const unsigned char *, unsigned char *);
long amfm_align(const unsigned char *, long, unsigned char *, long);
#endif
