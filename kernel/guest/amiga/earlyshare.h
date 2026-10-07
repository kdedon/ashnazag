#ifndef MIG_EARLYSHARE_H
#define MIG_EARLYSHARE_H
/*
 * Files startmig copies from SYS: into the boot extension, so the board
 * binds before DOS.  Names are SYS: paths in lower case; offsets are from
 * MIG_EARLY_BASE.
 */
#define MIG_EARLY_BASE 0x00f10000UL
#define MIG_EARLY_END 0x00f7f000UL
#define MIG_EARLY_MAGIC 0x4d494745U
#define MIG_EARLY_FILES 12
struct mig_early_file {
    char name[40];
    unsigned int offset, size;
};
struct mig_early {
    unsigned int magic, count;
    struct mig_early_file file[MIG_EARLY_FILES];
};
/* the tool types of the board's monitor icon, each NUL-terminated */
#define MIG_EARLY_TOOLTYPES "tooltypes"
/* the Workbench mode: display ID, width, height, depth, control */
#define MIG_EARLY_SCREENMODE "screenmode"
/* present: open Workbench and a screen in front of it before DOS, for tests */
#define MIG_EARLY_SCREENS "screens"

/* the guest's progress, in the RTG page after the paravirtual words */
#define MIG_EARLY_STATUS 0x20001fc0UL
#define MIG_EARLY_NONE 0
#define MIG_EARLY_STARTED 1
#define MIG_EARLY_NOLIB 2       /* a library's initialization failed */
#define MIG_EARLY_NOBOARD 3     /* the board did not bind */
#define MIG_EARLY_BOUND 4
struct mig_early_status {
    volatile unsigned int status, step;
    /* DOS functions called before DOS existed: bit n is LVO -6n */
    volatile unsigned int calls[8];
    /* the first screens moved to the board: the mode asked for, with bit
     * 31 once opened in the board's mode and depth, and the screen */
    volatile unsigned int screens, screen[4];
};

/*
 * An alert for the display helper to draw, in the input page after the
 * queue: the guest bumps seq, rings and waits until done equals it.  text
 * holds the alert's lines, each ended by a newline.  click is the button
 * pressed while it shows; answer is nonzero for the left one.
 */
#define MIG_EARLY_ALERT 0x21000c80UL
struct mig_early_alert {
    volatile unsigned int seq, done, answer, click;
    unsigned int number, height, frames;
    char text[600];
};
#endif
