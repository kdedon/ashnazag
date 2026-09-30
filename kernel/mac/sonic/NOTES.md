# SONIC (DP83932) on the Quadra 800

## Files

| File | Role |
|---|---|
| `snreg.h` | registers, bits, 32-bit-mode descriptor layouts |
| `sonic.h`, `sonic.c` | chip core: pool, rings, CAM, tx/rx, interrupt, watchdog. No STREAMS; builds on the host with `-DSN_HOST` |
| `sndlpi.c` | STREAMS DLPI provider `sninfo`, AEN-compatible ioctls, bring-up, `snintr` |
| `snsup.s` | `sn_dcpush`/`sn_dcinval` (68040 line push/invalidate) |
| `test/` | behavioural SONIC model + host tests |
| `verify.sh` | image checks + host tests |

## How AMIX networks plug in (from the link kit)

- `master.d/kernel.c`: `cdevsw[18]` = `&aeninfo` (Amiga A2065 LANCE driver `aen`); `aenintr` sits in `int2_tbl` (Amiga level-2 chain).
- `aen` is a DLPI style-1 connectionless provider: `DL_INFO_REQ`, `DL_BIND_REQ` (sap = Ethernet type), `DL_UNBIND_REQ`, `DL_UNITDATA_REQ/IND`. Minor = board (bits 0–3) | stream slot+1 (bits 4–7); slot 0 picks the next free one, so one node serves ip and arp. `DL_UNITDATA_IND` puts dst and src (6 bytes each) at `sizeof(union DL_primitives)`. It links an `ifstats` entry and ACKs `SIOCSIFFLAGS/SIOCGIFFLAGS`.
- `/etc/inet/strcf` `addaen /dev/aen0 aen0`: opens the node twice; one stream (with `app` pushed) is `I_LINK`ed under `/dev/ip`, the other under `/dev/arp`; ip binds 0x0800, arp 0x0806. `network-config` first runs `/usr/amiga/bin/aen -S` (`AEN_GET_STATUS`) as a liveness test.
- `sn` keeps all of that: `cdevsw[18]`, same minor scheme, same address layout, same `AEN_*` ioctls and structures (`aenuser.h` from the kit), so `/dev/aen0` = `c 18 0` and the stock scripts work unchanged. `sn_ifname` ("aen") names the `ifstats` entry.

## DLPI additions (for an AppleTalk link layer)

| Bind | Frames |
|---|---|
| sap > 1500 | Ethernet II of that type |
| sap 2..0xFE, even | 802.3 + LLC UI, DSAP = sap (tx DSAP from 7th address byte if given) |
| sap 0xAA + `DL_SUBS_BIND_REQ` (5 bytes OUI+type, up to 2) | SNAP. AppleTalk: `08 00 07 80 9B`; AARP: `00 00 00 80 F3` |

- `DL_ENABMULTI_REQ` 0x1d / `DL_DISABMULTI_REQ` 0x1e / `DL_PHYS_ADDR_REQ` 0x31 (DLPI 2.0 numbers; this `dlpi.h` lacks them). Multicast addresses go into CAM entries 1–15, reference counted, 4 per stream. Non-broadcast multicast frames reach only streams that enabled them.
- SNAP tx: 11-byte address (MAC + OUI/type) or the stream's first subs-bind. 802.3 length field written from the payload; rx trims to it.
- Not supported: trailers (dropped), `DL_ATTACH_REQ` (style 1), promiscuous primitives (use `AEN_SET_CONFIG` mode `PROM`).

## Hardware facts used

| Item | Value | Source | Status |
|---|---|---|---|
| Registers | 0x50F0A000, reg n at +4n+2, 16-bit | NetBSD mac68k `if_sn_obio.c`; hw doc §2.2 | unverified on hardware |
| DCR | EXBUS\|BMS\|DW\|RFT1\|TFT0 = 0x803A; DCR2 = 0 | NetBSD (Q800 case + `DCR_DW` for 32-bit) | unverified |
| MAC PROM | 0x50F08000, 6 bytes, stride 1 | NetBSD `sn_get_enaddr` | unverified |
| Interrupt | VIA2 CA1 (IFR bit 1), slot $9 = VIA2 port A bit 0, active low | hw doc §3.1–3.2 (A/UX `slotintr`), NetBSD `add_nubus_intr(9)` | unverified |
| Byte order | big-endian bus: descriptor longwords with value in the low 16 bits; CAM port = byte0 \| byte1<<8 | NetBSD (`sc_bigendian`, `sonic_set_camentry`) | unverified |
| A/UX | no SONIC driver in this image (hw doc §4.7), nothing to cross-check | | |

Machine check: `mac_model` must be 35 (Gestalt Quadra 800); patch `sn_anymodel` to try others with the same layout.

### MAC address (hw doc open item)

`sn_getaddr`, first match wins:
1. `sn_eaddr[6]` patched nonzero.
2. PROM byte 0 = 0x10 → bit-reverse every byte (Apple stores canonical/Token-Ring order; 08 → 10).
3. PROM as read, if the OUI is Apple (08:00:07, 00:A0:40, 00:05:02).
4. PROM bit-reversed, if that gives an Apple OUI.
5. CAM entry 15, where the ROM driver leaves the address (read before the first reset).
6. PROM as read, if unicast and not all 0/FF.

The boot line prints which source was used.

## Memory and descriptor layout

`sn_pool` (69632 bytes, `.bss`), aligned at run time to 4 KB:

```
+0x0000  TDA  8 x 32 B   status config size nfrag fptr0 fptr1 fsize link
+0x0100  RDA 32 x 28 B   status count ptr0 ptr1 seq link inuse
+0x0480  RRA 32 x 16 B   ptr0 ptr1 wc0 wc1          (slot n = rx buffer n, fixed)
+0x0680  CDA 16 x 16 B + enable word                (entry port0 port1 port2)
+0x1000  tx buffers  8 x 1536
+0x4000  rx buffers 32 x 1536 (one RBA each)
```

All four areas share one 4 KB block, so each lies inside one 64 KB segment (UTDA, URDA, URRA supply the upper 16 bits; the CDA uses URRA).

- **One frame per RBA.** RBA = 768 words, EOBC = 760: after any frame (≥ 32 words) fewer than 760 words remain, so the chip sets LPKT and takes the next RBA; a 1518-byte frame (759 words) always fits.
- **RRA guard.** RWP starts at slot 31 (31 of 32 usable). After servicing, RWP = slot of the last buffer consumed; that buffer stays as the guard, every earlier one (including RBAs the chip skipped) returns.
- **RDA.** `inuse` = ownership (chip writes 0). A serviced descriptor gets EOL and `inuse = 1`, then EOL is cleared on its predecessor.
- **TDA.** New frame: its descriptor carries EOL, EOL is cleared on the previous one, then `CR = TXP`. After stopping at EOL the chip re-reads that link on the next TXP (NetBSD `sonic_start` relies on this). A reclaim that finds frames pending and `TXP` clear re-issues TXP (covers a TXP lost while the chip was stopping and the stop after FU/EXC).

## Register sequences

Init (`sn_init`):
```
IMR=0; CR=HTX|RXDIS|STP; wait TXP|RXEN|ST clear; ISR=7FFF
CR=RST; 1 ms; DCR=803A; DCR2=0; ISR=7FFF; CR=0; 1 ms
rings in memory, push
UTDA/CTDA, URDA/CRDA, URRA/RSA/REA/RRP/RWP, EOBC=760, RSC=0, tallies=FFFF
CDP/CDC, CR=LCAM, wait LCAM clear, ISR=LCD
RCR=BRD (|PRO); CR=RRRA, wait clear; ISR=7FFF; IMR; CR=RXEN
```
IMR = RFO|RBAE|RBE|RDE|TXER|PTX|PRX.
CAM change while running (`sn_filter`): `CR=RXDIS`, wait, LCAM, `RCR`, `CR=RXEN`.

Interrupt (`sn_intr`, up to 16 passes): read ISR&IMR, ack all but RBE/RDE, service receive, then ack RBE/RDE (the chip resumes only after the refill), then reclaim transmit.

Watchdog (`timeout`, 1 s): run `sn_intr` (rescues a lost CA1 edge), kick TXP after 2 s without tx progress, full re-init if the receiver dropped, after 5 s, or while a previous init failed.

## Interrupt dispatch

`p2int`: CB2 → `ncr96intr`; else CA1 → ack the edge (IFR=0x02), read port A (+0x1E00); bit 0 low → `snintr`; otherwise count `sn_nslot`. CA1 is edge-triggered on the combined slot lines, so the edge is acked before the lines are read and `sn_intr` leaves the SONIC line released. `sn_start` sets VIA2 PCR bit 0 = 0 (falling edge) and IER = 0x82.

A slot line held low by someone else hides later SONIC edges; the 1 s watchdog then keeps traffic alive at low rate. The ROM leaves DAFB's VBL interrupt on, so the video init masks it (DAFB +0x104 = 0, +0x10C clears a pending one) on a Quadra 800. The Q800 VIA2 has no slot-interrupt mask (that is RBV), so the source is the only place to stop it.

## Cache strategy

- The pool is kernel `.bss` below 1 GB. The Mac `pstart` maps supervisor data there through DTT0 = 0x003FA060: identity (VA = PA, so pointers are chip addresses) and cache-inhibited. No coherency work is needed.
- The driver still brackets every DMA hand-off: `sn_dcpush` (cpushl + cinvl per line) after the CPU writes a descriptor or tx buffer, `sn_dcinval` (cinvl) before reading a descriptor or rx frame. The pair per line follows the port's `dma_cache040` rule (060 CPUSH invalidation depends on CACR.DPI).
- Buffers are safe in any cache mode. Descriptors are safe cache-inhibited or write-through, not copyback: clearing EOL in an RDA link read-modify-writes the line holding that descriptor's `seq` and `in_use`, which the chip may be writing at that moment; the push would restore the stale `in_use` and stall the ring. RDA entries are 28 bytes, so neighbours share lines too. If DTT0 ever goes copyback, the descriptor block needs its own cache-inhibited mapping. `verify.sh` checks the DTT0 value in `pstart`.
- `SN_SYNC()` = `nop` before any register write that makes the chip look at memory, draining the 040 store buffer (DTT0 is non-serialized).
- Frames are copied between mblks and the pool, so mblk memory (paged, cacheable) never meets the chip.

## Compiler trap

`-traditional` erases `volatile`: registers use `__volatile__` (`SN_REG`), SR through `__volatile__` asm with `"memory"` (`sn_spl` raises to IPL 2, never lowers). Descriptor reads follow an out-of-line `sn_dcinval` call, which is the compiler barrier. Checked in the image by `verify.sh` (3 SR accesses in `sn_spl/sn_splx`).

## Host tests (`test/`)

Run with `sh kernel/mac/sonic/verify.sh` (image checks, then the host tests).

`simsonic.c` models CR commands, RRA fetch with RRP/RWP/REA wrap, EOBC/LPKT, RBE stall and resume on ISR clear, RDE stall and link re-read, RBA-exceeded, TDA walk with EOL restart, CAM load with enable word, address filtering; fault injection for a lost TXP and a FIFO underrun. Chip addresses start at a non-4 KB-aligned base to exercise the carve. 42 checks: init registers, CAM load/reload and filtering, 163 frames over 5 ring wraps, exhaustion (31 held, rest missed, full recovery), oversized frame, tx ring full/partial/restart, padding to 60, lost TXP (watchdog), FU (reclaim restart), watchdog re-init. RDE cannot occur before RBE with 32 descriptors for 31 buffers, so it is modelled but not exercised.

The model encodes our reading of the chip; only hardware can confirm it.

## Hardware checks (open)

1. Boot line: `sn0: SONIC rev …` and an address with an Apple OUI; the source name says which PROM order held. Compare with the Mac OS Network control panel.
2. `/usr/amiga/bin/aen -S` shows Running; `ifconfig aen0`, ping in both directions.
3. `sn_nintr` and `sn_nslot` (kmem): `sn_nintr` counts `snintr` calls from `p2int`, so it grows with traffic when CA1 works. A few `sn_nslot` are normal (an edge whose event `sn_intr` already served); a steady 60–75 Hz means a video VBL still holds a slot line.
4. Missed/RBE counts under a flood (`aen -S` Packets Missed): rx keeps working afterwards.
5. `txkick` stays near 0; `txfu` 0 (DCR thresholds / BMS on this bus).
6. Frames of 60 and 1514 bytes both ways; multicast via `DL_ENABMULTI_REQ` once an AppleTalk layer exists.
7. DCR: if the chip bus-errors or hangs, try without BMS.

## Sources

The AMIX link-kit driver and headers (`aen.c`, `aenuser.h`, `dlpi.h`, `master.d/kernel.c`, `strcf`), the DP83932 register/descriptor definitions and init order as published in NetBSD 10.1 `dev/ic/dp83932*.{c,h}` and `arch/mac68k/{dev/if_sn.c,obio/if_sn_obio.c}` (facts only; the code is new), `docs/mac-q800-hardware.md` and the port's `dma_cache040` notes.
