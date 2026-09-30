# ROM compressed A-trap dispatch table

Derived from the ROM images "368CADFE - Mac IIci.ROM" (512 KB) and
"420DBFF3 - Quadra 700&900 & PB140&170.ROM" (1 MB), both version $067C. Decoder: `tools/romdisp.py`.

## Header fields

| Offset | IIci       | Quadra     | Meaning |
|--------|------------|------------|---------|
| $16    | $00000044  | $00000044  | offset of the foreign-OS entry table |
| $22    | $000514E0  | $000CA0E0  | offset of the encoded dispatch table |

Foreign-OS table at $44 (same in both): entry 0 = $9A96 (table
initialiser), entry 1 = $99B0 (A-trap dispatcher), entry 2 = $9AE6
(unimplemented-trap handler), then $5DE0, $CC60, $5C.

## Entry 0 ($9A96), identical code in both ROMs

```
9A96  lea     $9AE6(pc),a4      ; a4 = unimplemented-trap handler
9A9A  movea.l $2AE.w,a3         ; a3 = ROMBase (low memory)
9A9E  movea.l a3,a2             ; a2 = running pointer, starts at ROMBase
9AA0  lea     $E00.w,a1         ; a1 = Toolbox table
9AA4  movea.l a3,a0
9AA6  adda.l  $22(a0),a0        ; a0 = ROMBase + header[$22]
9AAA  moveq   #$7F,d2
9AAC  move.w  #$3FF,d0          ; 1024 Toolbox slots
9AB0  move.b  (a0)+,d1          ; next: fetch first byte
9AB2  bmi.b   $9ACA             ;   bit 7 set -> one-byte forms
9AB4  lsl.w   #8,d1
9AB6  move.b  (a0)+,d1          ; d1.w = 15-bit big-endian value
9AB8  add.w   d1,d1             ; words -> bytes
9ABA  beq.b   $9ADC             ; zero -> finished
9ABC  adda.w  d1,a2             ; a2 += sign-extended word
9ABE  move.l  a2,(a1)+          ; store slot
9AC0  dbra    d0,$9AB0
9AC4  lea     $400.w,a1         ; Toolbox done: switch to OS table
9AC8  bra.b   $9AB0
9ACA  and.w   d2,d1             ; low 7 bits
9ACC  beq.b   $9AD8             ;   $80
9ACE  cmp.w   d2,d1
9AD0  bne.b   $9AB8             ;   $81..$FE -> short delta
9AD2  movea.l (a0)+,a2          ;   $FF: next long
9AD4  adda.l  a3,a2             ;        + ROMBase
9AD6  bra.b   $9ABE
9AD8  move.l  a4,(a1)+          ; $80: store unimplemented handler
9ADA  bra.b   $9AC0
9ADC  lea     $99B0(pc),a0      ; IIci only; Quadra has bra.l $8500C
9AE0  move.l  a0,$28.w          ; vector $28 = dispatcher, rts
```

On the Quadra, $9ADC is `bra.l $8500C`, which computes the same $99B0
(`lea $FFF849A4,a0; lea (pc,a0.l)`), stores it at $28, and if CPUFlag
($12F) = 4 sets $6F4 to a `cpusha` routine at $85030 before `rts`.

## Encoding

A running pointer P starts at ROMBase. The byte stream is read
sequentially; each entry fills the next slot with an absolute address
(ROMBase + offset):

| Bytes           | Action |
|-----------------|--------|
| `0hhh hhhh  ll` (first byte $00-$7F) | v = 15-bit value; if v = 0: end. Else P += sign-extend16(v*2), slot = P |
| `$80`           | slot = unimplemented handler ($9AE6); P unchanged |
| `$81`-`$FE`     | P += (byte & $7F) * 2, slot = P |
| `$FF` + long L  | P = ROMBase + L, slot = P |

Notes:
- Doubling is done in 16 bits and `adda.w` sign-extends, so two-byte
  values $4000-$7FFF are backward deltas (-$8000..-2 bytes). Both ROMs
  use them (227 / 226 times); a decoder that treats them as unsigned is
  wrong.
- The one-byte delta range is +2..+$FC bytes; there is no one-byte
  backward delta.
- `$80` does not touch P, so the next delta is relative to the last
  real entry.

## Sequencing and termination

The first 1024 entries fill the Toolbox table at $E00 (slot n = trap
$A800+n). When `dbra` expires, a1 switches to $400 and d0 is $FFFF, so
the OS table (slot n = trap $A000+n) is filled until the `$00 $00`
terminator; there is no count check for it. Both ROMs contain exactly
1024 + 256 entries before the terminator (IIci table $514E0-$51D6D,
2190 bytes; Quadra $CA0E0-$CA986, 2215 bytes). The terminator also
ends the Toolbox part if it came early.

The dispatcher at $99B0 confirms the slot mapping: for trap >= $A800 it
loads `$1E00 + (trap-$AC00)*4` = `$E00 + (trap-$A800)*4`; for OS traps
it uses the low byte, `[$400 + (trap & $FF)*4]`.

## Counts

| ROM    | 2-byte delta | 1-byte delta | $FF absolute | $80 unimpl |
|--------|--------------|--------------|--------------|------------|
| IIci   | 460          | 435          | 112          | 273        |
| Quadra | 457          | 435          | 119          | 269        |

## Cross-checks

- $9AE6 (unimplemented): `movem.l d0-d7/a0-a7,$C30.w; moveq #12,d0;
  jmp $2726` — SysError 12, dsCoreErr (Inside Macintosh).
- _GetResource ($A9A0) -> $1B5A0 in both: reads a word then a long from
  the argument pointer and clears the long above them, matching Pascal
  `GetResource(theType, theID): Handle`.
- _NewPtr ($A01E) -> $D360 (IIci): `move.l #$40,-(sp); move.l $1EE8,-(sp);
  rts`, i.e. a selector pushed and a jump through a low-memory vector.
- All decoded offsets lie inside the ROM image.
- Slot $200 of the Toolbox table ($AA00) in the IIci decodes to $300F0,
  followed by a dense run $AA00-$AA66 and $AA90-$AAA2: the Color
  QuickDraw range that Inside Macintosh V numbers from _OpenCPort
  ($AA00). This confirms 1024 Toolbox slots ($A800-$ABFF) followed by OS
  slots from $A000.
