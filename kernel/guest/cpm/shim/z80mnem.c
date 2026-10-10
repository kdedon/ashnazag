/* Instruction names and condition codes for the CP/M-80 shim.  Separate
 * from the decoder because the target runs the assembly one: were these
 * still beside it, the whole reference decoder and its tables would be
 * linked into a program that never calls them. */

#include "z80.h"

/*
 * The ED members that have a name here.  The group is sparse and its
 * regularity is in three separate places at once -- the 16-bit
 * arithmetic and the two indirect loads share the 0x40-0x7F block with
 * NEG and the interrupt forms, and the block operations sit in a 4x4
 * grid at 0xA0-0xBB -- so it is written out rather than computed.
 *
 * `cpi' here is the Z80's BLOCK COMPARE, not the 8080's compare
 * immediate: the latter is spelled "cmp" (alu[7]) after the 8080
 * convention below, so the two never collide in output even though the
 * assembler mnemonics do.
 *
 * Anything not named is "ed-group": decoded for its length, refused by
 * the executor, and reported as a worklist entry rather than run.
 */
static char *edmnem(sub)
int sub;
{
	static char *blk[16] = {
		"ldi",  "cpi",  "ini",  "outi",
		"ldd",  "cpd",  "ind",  "outd",
		"ldir", "cpir", "inir", "otir",
		"lddr", "cpdr", "indr", "otdr"
	};

	if (sub >= 0xa0 && sub <= 0xbb && (sub & 7) < 4)
		return (blk[(((sub - 0xa0) >> 3) << 2) | (sub & 3)]);
	if (sub >= 0x40 && sub <= 0x7f) {
		switch (sub & 15) {
		case 0x02: case 0x0a:
			return ((sub & 8) ? "adc hl" : "sbc hl");
		case 0x03: case 0x0b:
			return ((sub & 8) ? "ld rp,(nn)" : "ld (nn),rp");
		case 0x04: case 0x0c:
			return ("neg");
		}
	}
	return ("ed-group");
}

/*
 * z80mnem -- the mnemonic for a decoded instruction, for a refusal
 * message.
 *
 * The 8080 spelling is used, to match the rest of this tree -- `mov'
 * not `ld', `jz' not `jp z'.  The four Z80 base opcodes have no 8080
 * spelling and keep Zilog's.
 */
char *z80mnem(in)
struct z80in *in;
{
	static char *alu[8] = {
		"add", "adc", "sub", "sbb", "ana", "xra", "ora", "cmp"
	};
	static char *cc[8] = {
		"nz", "z", "nc", "c", "po", "pe", "p", "m"
	};
	static char *rot[4] = { "rlc", "rrc", "ral", "rar" };
	static char *jcc[8] = {
		"jnz", "jz", "jnc", "jc", "jpo", "jpe", "jp", "jm"
	};
	static char *ccc[8] = {
		"cnz", "cz", "cnc", "cc", "cpo", "cpe", "cp", "cm"
	};
	static char *rcc[8] = {
		"rnz", "rz", "rnc", "rc", "rpo", "rpe", "rp", "rm"
	};
	static char *jr[5] = { "jrnz", "jrz", "jrnc", "jrc", "jr" };
	static char *cbrot[8] = {
		"rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"
	};
	static char *cbgrp[4] = { "", "bit", "res", "set" };

	switch (in->op) {
	case Z_NOP:	return ("nop");
	case Z_LDRR:	return ("mov");
	case Z_LDRI:	return ("mvi");
	case Z_ALU:	return (alu[in->x & 7]);
	case Z_INR:	return ("inr");
	case Z_DCR:	return ("dcr");
	case Z_LXI:	return ("lxi");
	case Z_DAD:	return ("dad");
	case Z_INX:	return ("inx");
	case Z_DCX:	return ("dcx");
	case Z_LDAX:	return ("ldax");
	case Z_STAX:	return ("stax");
	case Z_LDA:	return ("lda");
	case Z_STA:	return ("sta");
	case Z_LHLD:	return ("lhld");
	case Z_SHLD:	return ("shld");
	case Z_ROT:	return (rot[in->x & 3]);
	case Z_DAA:	return ("daa");
	case Z_CMA:	return ("cma");
	case Z_STC:	return ("stc");
	case Z_CMC:	return ("cmc");
	case Z_JMP:	return ("jmp");
	case Z_JCC:	return (jcc[in->x & 7]);
	case Z_CALL:	return ("call");
	case Z_CCC:	return (ccc[in->x & 7]);
	case Z_RET:	return ("ret");
	case Z_RCC:	return (rcc[in->x & 7]);
	case Z_RST:	return ("rst");
	case Z_PCHL:	return ("pchl");
	case Z_SPHL:	return ("sphl");
	case Z_XTHL:	return ("xthl");
	case Z_XCHG:	return ("xchg");
	case Z_PUSH:	return ("push");
	case Z_POP:	return ("pop");
	case Z_IN:	return ("in");
	case Z_OUT:	return ("out");
	case Z_EI:	return ("ei");
	case Z_DI:	return ("di");
	case Z_HLT:	return ("hlt");
	case Z_JR:	return (jr[in->x < 5 ? in->x : 4]);
	case Z_DJNZ:	return ("djnz");
	case Z_EXAF:	return ("ex af,af'");
	case Z_EXX:	return ("exx");
	case Z_CB:
		/* Zilog's spelling, because the CB group has no 8080
		 * one at all: these encodings do not exist on an 8080.
		 * The four rotate names collide with the base map's
		 * (0x07 is also "rlc") and that is correct -- CB 07 is
		 * `RLC A' and does the same thing to A that 0x07 does,
		 * differing only in the flags. */
		if (in->x == 0)
			return (cbrot[in->imm & 7]);
		return (cbgrp[in->x & 3]);
	case Z_ED:
		return (edmnem((int)in->sub));
	case Z_IX:	return (in->pfx == 0xdd ? "ix-group" : "iy-group");
	case Z_IXCB:	return (in->pfx == 0xdd ? "ixcb-group" : "iycb-group");
	case Z_HOOK:	return ("hook");
	}
	return ("(bad)");
}

/* The condition codes, in the encoding order the opcode's middle field
 * gives: NZ Z NC C PO PE P M.  Exported because the executor's Jcc, Ccc
 * and Rcc all need it and the JR forms need its first four; a second
 * copy would be a second thing to get wrong. */
int z80cond(f, cc)
int f, cc;
{
	register int t;

	switch (cc >> 1) {
	case 0:  t = (f & F_ZE) != 0; break;		/* Z	*/
	case 1:  t = (f & F_CY) != 0; break;		/* CY	*/
	case 2:  t = (f & F_PA) != 0; break;		/* PE	*/
	default: t = (f & F_SI) != 0; break;		/* M	*/
	}
	return ((cc & 1) ? t : !t);
}
