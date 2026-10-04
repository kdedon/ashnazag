# Booting the Mac kernel from A/UX Startup

Stock A/UX Startup 3.1 and its `launch` load the kernel. The ELF is wrapped as an A/UX COFF kernel.

```
unix-mac.elf ──elf2coff──> unix-mac.coff
  pstart  PA 0x4000  28-byte stub copied out of .text (aux_pstart..aux_pstart_end); entry
  .text   PA=VA 0x10000
  .data   PA=VA
  .bss    PA=VA  (launch neither loads nor zeroes it)

launch places:   PA 0x0000-0x1FFF  Mac low memory (copied onto itself)
                 PA 0x3C00         'Pigs' info block + drive-queue copy
sash jumps:      0x4000, IPL 7, MMU off, d0 = 0x536D7201, a0 = 0x3C00

pstart stub: .text PA == VA (info +0x90 == +0x8C)?  no -> stop
             yes -> jmp aux_entry (absolute)
aux_entry:   SP own; cpusha, CACR/TC/ITT0-1/DTT0-1 = 0, pflusha; zero BSS;
             VBR -> private table (prints vector + PC); VIA1/VIA2 IER = 0x7F;
             SCC init, banner; checks d0, 'Pigs', machine 33, CPUFlag 4;
             writes the boot record at end (even); jmp mac_entry (d0 = 0)
mac_entry:   as for a direct -kernel boot: finds the record at end, stext
```

## Boot record written by aux_entry

| Tag | Value | Source |
|---|---|---|
| BI_MACHTYPE | 3 (MACH_MAC) | |
| BI_CPUTYPE, BI_MMUTYPE | 68040 (4) | low-memory `CPUFlag` $12F must be 4 |
| BI_FPUTYPE | 68040 (4), only if `HWCfgFlags` ($B22) bit 12 is set | |
| BI_MEMCHUNK | {0, (djMEMC 0x50F0E02C & 0xFF) << 22} | |
| BI_MAC_MODEL | info +0xB0 + 2 (Gestalt 'mach', 35) | |
| BI_MAC_MEMSIZE | RAM in MB | |
| BI_MAC_ROMBASE | `ROMBase` $2AE | |
| BI_MAC_VIA1BASE | `VIA` $1D4 if it is in 0x50Fxxxxx, else 0x50F00000 | |
| BI_MAC_SCCBASE | `SCCRd` $1D8 if it is in 0x50F0C000–3F, else 0x50F0C020 | |
| BI_MAC_VADDR, BI_MAC_VROW | `ScrnBase` $824, `ScreenRow` $106 & 0x3FFF, only if both are nonzero | no depth/dims in the hand-off |
| 0x8F00 (private) | info-block PA, hand-off d0, low-memory PA | read by `mac_diskpick` (root/swap/flags) |

Low memory is read at info − 0x3C00, which is the copy launch made.

## Rules launch enforces

`elf2coff -c` checks every one of these rules. It accepts A/UX's own `/unix` and our output.

| Rule |
|---|
| f_magic 0x150, otherwise "Can't load type %o files" |
| The a.out magic is not checked. We use 0410, as `/unix` does |
| f_opthdr must be 28 |
| At most 8 section headers |
| Headers are sorted by s_scnptr, then read in that order |
| No gap between loaded sections in the file: after a gap the next section loads shifted |
| Loaded: `(s_flags & 0x1F) == 0`. STYP_BSS is skipped (not loaded, not zeroed) |
| Section named `pstart`: info → s_paddr − 0x400, low memory → s_paddr − 0x4000. If s_paddr is 0x500 or 0x2000 → info at 0x400, low memory at 0. With no `pstart`, both go to 0 |
| `.text`/`.data`/`.bss` (exact names): paddr += bank offset; vaddr/paddr/size go to info +0x8C…+0xAC |
| Bank offset = bank-1 base only if bank 1 exists and bank 0 ≤ 2 MB (never on a Q800) |
| Entry = a.out entry (not checked against pstart) |
| No `MODULES` section → autoconfig command OK, no board check |
| A path equal to `newunix` forces autoconfig |
| Without `-s`: a 2-byte zero is written at roundup(max section end, 4 KB). That is our `end`; the boot record overwrites it later |
| Info: 'Pigs', version byte 1, flags +0xB6 (1 = -v, 0x10 = -S, 2/4 = parity, 8 = -e/-p, plus -k) |
| d0 = 0x536D7200 \| version. The flags mask defaults to 0, so version = 1 |

Option letters: a b d e f k m n p r s S v, as in the help text. So with `-m` the path is a plain Mac path relative to the default volume, not `(mac):…`. `(mac):` is sash's own device prefix, for paths given without `-m` (sash `PATH` = `(mac):|(mac):bin:`).

## On the Quadra 800

1. Copy `unix-mac.coff` as raw data fork (no MacBinary) next to A/UX Startup, e.g. as `unix.coff`. Type 'COFF', creator 'SASH' is cosmetic.
2. Serial cable on the modem port (SCC channel A), 9600 8N1.
3. In A/UX Startup, cancel autolaunch (Command-period), then:

```
launch -m -n unix.coff
```

`-n` is not required, since there is no `MODULES` section, but keeps autoconfig out of it. The full-path form also works: `launch -m -n "Macintosh HD:unix.coff"`. Without `-m`: `launch -n (mac):unix.coff`. `-d` dumps the info block and load layout before launching. `-v`/`-S` only set info flags, which the kernel ignores.

Expected serial output: `A/UX Startup hand-off: d0 0x536d7201 info 0x00003c00`, machine 0x21, CPUFlag 0x4, djMEMC RAM, then the `AMIX/Mac entry shim` banner and `config:` lines, as under direct boot.

## Open hardware questions

- Does the whole 1.16 MB file fit in sash program space (else "Not enough program space")? A/UX's `/unix` is 0.84 MB.
- Low-memory values on a Q800 to confirm: `SCCRd`, `VIA`, `ScrnBase` as a physical address, `ScreenRow`, `HWCfgFlags` FPU bit.
- Does djMEMC 0x50F0E02C × 4 MB equal installed RAM? MemTop is printed next to it for comparison.
- TT registers are cleared before anything touches memory-mapped I/O; until then the 68040 runs with MMU off and the Mac OS TT values, accessing only `.data`.

## Limits

- Quadra 800 (machine 33) and a 68040 only. Other machines halt with a message: their RAM controllers and SCC addresses differ.
- If `.text` was moved (split-bank case), the stub stops silently: it cannot reach the SCC code.
- No video depth or dimensions (the hand-off has none).
- The kernel relies on nothing in BSS before `aux_entry` zeroes it (launch does not zero BSS).
- Any exception before `mac_entry` prints "auxentry: exception, vector offset … pc …" and halts.

## Testing

Static: `elf2coff -c` checks the launch rules; parse the COFF back with `coff2elf` (`.text`/`.data` must be byte-identical to the ELF).

