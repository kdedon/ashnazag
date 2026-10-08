/*
 * w16ftopt.h -- FreeType's options for the Win16 environment's TrueType
 * (ttf.c), in place of FreeType's own ftoption.h: TrueType outlines
 * hinted by their own programs with the classic (v35) interpreter, as
 * Windows 3.1's rasterizer ran them (no subpixel hinting), scan converted
 * in black and white.  Nothing else: no compressed or other font
 * formats, no embedded bitmaps (Windows 3.1 did not use them), no
 * assembler, so it builds as plain C89 for the 68k.
 */
#ifndef FTOPTION_H_
#define FTOPTION_H_

#include <ft2build.h>

FT_BEGIN_HEADER

#define FT_CONFIG_OPTION_NO_ASSEMBLER
#define FT_RENDER_POOL_SIZE	16384L
#define FT_MAX_MODULES		8

#define TT_CONFIG_CMAP_FORMAT_0
#define TT_CONFIG_CMAP_FORMAT_4
#define TT_CONFIG_CMAP_FORMAT_6

#define TT_CONFIG_OPTION_BYTECODE_INTERPRETER
#define TT_CONFIG_OPTION_MAX_RUNNABLE_OPCODES	1000000L
#define TT_USE_BYTECODE_INTERPRETER

/* the base's PostScript helpers are built though no PostScript font is read */
#define T1_MAX_CHARSTRINGS_OPERANDS	256
#define T1_MAX_DICT_DEPTH	5
#define T1_MAX_SUBRS_CALLS	16

FT_END_HEADER

#endif
