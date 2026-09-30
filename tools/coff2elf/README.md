# coff2elf

Converts AT&T COFF into ELF32: Coherent i386 (the commodore-900-toolchain
format) and m68k (A/UX).

| f_magic | input | output |
|---|---|---|
| 0x14C | Coherent i386 COFF object, LE | ET_REL, EM_386, `.rel.*` (REL) |
| 0x150 | m68k COFF object, BE | ET_REL, EM_68K, `.rela.*` (RELA) |
| 0x150 | m68k COFF executable (F_EXEC), BE | ET_EXEC, EM_68K, PT_LOADs |

```
coff2elf [-u] [-x] [-l] [-R root] in out
  -u  strip one leading underscore from symbol names
  -x  write ET_EXEC even without F_EXEC (e.g. a shared library)
  -l  ET_EXEC: also load the static shared libraries named in .lib
  -R  directory .lib path names are resolved under
```

K&R C; COFF is read by byte offsets, so it builds with a modern LP64 cc
and an old ILP32 native cc.

## Mapping

- **Sections**: every COFF section becomes ELF section of the same number.
  TEXT → `AX`, DATA → `WA`, BSS → NOBITS `WA`; DSECT, no file data or pure
  BSS → NOBITS; other kinds (`.lib`, `.comment`, INFO) → non-alloc PROGBITS.
- **Symbols**: C_EXT/C_EXTDEF global; C_STAT/C_LABEL local; debug classes,
  C_FILE, N_DEBUG and aux entries skipped. Section-named C_STAT symbols
  become the ELF section symbol. Undefined with a value → SHN_COMMON
  (value = size). Symbols in a DSECT (A/UX `.lowmem`) → SHN_ABS. ET_REL
  values are section-relative; ET_EXEC values stay absolute.
- **Relocations** (ET_REL). COFF keeps the addend in place, computed
  against the symbol's COFF value; A = field − value(sym), plus r_vaddr if
  PC-relative. m68k moves it to `r_addend` and zeroes the field; i386 keeps it
  in place.

  | COFF r_type | meaning | ELF |
  |---|---|---|
  | 0x11 R_RELLONG (also 0x06 R_DIR32) | 32-bit absolute | R_68K_32 / R_386_32 |
  | 0x14 R_PCRLONG | 32-bit PC-relative (field = target − field address) | R_68K_PC32 / R_386_PC32 |
  | 0x10 R_RELWORD, 0x0F R_RELBYTE | 16/8-bit absolute | R_68K_16 / R_68K_8 |
  | 0x13 R_PCRWORD, 0x12 R_PCRBYTE | 16/8-bit PC-relative | R_68K_PC16 / R_68K_PC8 |

  Only 0x11 and 0x14 occur in A/UX files: 0x11 on `jsr abs.l` (`4eb9`),
  `move #imm` and data pointers; 0x14 on `bsr.l` (`61ff`) in `libmac1_s`.
  The 8/16-bit forms are untested.
- **Executables**: sections at their COFF `s_vaddr`, `p_paddr` = `s_paddr`,
  entry from the a.out header. Each loaded section (TEXT/DATA/BSS, not
  DSECT/NOLOAD, size > 0, any name: `pstart`, `MODULES`) gets a PT_LOAD;
  a NOBITS section starting where a segment ends joins it. File offsets are
  congruent with addresses modulo 4K. Relocations are not converted.
- **`-l`**: `.lib` holds 64-byte NUL-padded paths on A/UX (SVR3 word-counted
  entries also accepted). Each library's TEXT/DATA/BSS are added as
  `.text.<lib>` etc. at their fixed addresses with their own PT_LOADs, and
  its symbols join the table. The executable's NOLOAD placeholders
  (`.lt*`, `.ld*`, `.lb*`) are kept as NOBITS; a size mismatch with the
  library is warned about.

## Files

- `coff2elf.c` — the converter.
- `check.py` — independent check of an m68k conversion: section bytes,
  symbols, each relocation's symbol/addend, entry and segment coverage.
- `test.sh` — `make test [AUXROOT=<A/UX root>]`: i386 fixtures built,
  linked and run (needs `gcc -m32`); m68k kernel, drivers, executables and
  shared libraries converted, checked with `readelf` and `check.py`.
- `mkfix.c`, `libcoh/` — i386 fixture generator and link glue.
