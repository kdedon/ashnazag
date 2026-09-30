# otbridge: Open Transport bridge (kernel side)

Loadable module `otbridge`: the STREAMS module `otxti`, pushed on AMIX `/dev/tcp` and `/dev/udp` streams, translates Open Transport's TPI dialect to the host's; `/dev/otbridge` (char major 55, clone on minor 0) holds sessions with an event ring in the Mac process. Design: `docs/ot-passthrough-design.md` §3.

## Files

| File | Role |
|---|---|
| `otwire.h` | OT dialect (primitives, XTI options, errors) and the record, ioctl and ring formats shared with the Mac relays |
| `otbridge.h` | translation-table types, byte access, kernel endpoint and session state |
| `otxlate.c` | primitive and address translation, errno/TLI maps, XTI option engine; pure, also built on the host (`-DOTB_HOST`) |
| `otxti.c` | the STREAMS module: record cutting, translation, option engine driving, address bookkeeping, `OTX_ATTACH`, events |
| `otbdev.c` | sessions, ring (soft-locked pages written through the identity window while the owner maps them), SIGPOLL, stats, DLM linkages |
| `otbst.c` | stations: minor 1 clones, frame queue, read/write/poll, `OTB_STATION`/`STSIG`/`STMULTI`/`STSTATS` |
| `vstation.h` | the NIC side of stations (`vst_ops`, filled by the SONIC driver) |
| `test/otxtest.c` | host tests against hand-built records (43 checks) |
| `build.sh` | host tests; with `-k kernel.elf`, the module via `mkmod` |
| `*.diff` | `-p1` diffs from the repository root (below) |

## Wire format

- Records, both directions: `{u_char type; u_char flags; u_short ctllen; u_long datalen; ctl; data}`. Down: one `write` per record. Up: one record per `read` (`otxti` sets `RMSGN` at push); messages over 8 KB continue in records with `ctllen` 0 and `OTB_RF_MORE`.
- Down: bare data records become `M_DATA`; TPI records are translated, their options negotiated first (one host `T_OPTMGMT_REQ` per option), then sent without options.
- `T_CONN_RES` names the acceptor by its cookie in the `QUEUE_ptr` word; `otxti` substitutes the acceptor stream's driver read queue (same session only).
- `T_ADDR_REQ` is answered locally from the bind ack, connect request/confirm and connect indication. A socket bound to `INADDR_ANY` reports host 0.
- Ring: 16-byte header (magic, entries, head, tail, overflow flag) and 510 entries `{cookie, events}`; one live entry per endpoint, later events or'ed in; the Mac takes events with `cas` and advances tail; signal (default SIGPOLL 22) on empty → non-empty only.
- Before each post the kernel checks that the owner (same process, same address space) still maps the locked pages at the ring's address. If not (copy-on-write after a fork, exec, unmap, exit), the ring is lost: posts stop and the owner gets the signal once. `OTB_GETRING` returns 1 while the ring is live; `OTB_SETRING` registers it again.

## Stations (`/dev/otbstation`, minor 1)

- Each open clones a station (minors 64–67). `OTB_STATION {mac, mode}`: mode `OTB_ST_BRIDGE`; address 0 gives the derived one (02, the host address's last three bytes, the uid) and returns it; other addresses need root. Other users need group `otb_stgid` (25, display, as for the Mac session's screen) or `otb_stuser` (default 0), and get one station each (`EBUSY` otherwise); `/dev/otbstation` is 660 root:display.
- One frame per `read` (truncated to the buffer) and per `write` (14–1514 bytes, own source address, else `EINVAL`). Reads block, or return 0 with `O_NDELAY` and `EAGAIN` with `O_NONBLOCK`; `poll` gives `POLLIN`. Up to 8 frames queue; more are dropped (`ss_drop`). A blocking write waits up to 10 ticks for transmit room; `POLLOUT` only with room.
- `OTB_STSIG sig`: that signal to the caller when the queue becomes non-empty. `OTB_STMULTI {addr, on}`: up to 4 multicast addresses.
- The NIC side is `vst_ops` (weak: no SONIC, no stations, `ENXIO`).
- Under the A/UX personality, `'o'` ioctls on a major-55 descriptor pass through unchanged (`auxconv.c`).

## OT dialect (Open Transport 1.3)

- `tcp_wput_proto` switches on 101 (bind), 102 (connect; `AF_DNS` 42 goes to the resolver), 103, 105, 107, 108, 109, 110, 111, 112 (address), 124, 142 (OT-private resolve). Acks built: 122 bind, 127 error (16 bytes), 129 info (44 bytes, same 11 fields), 135 address (20-byte header + two 16-byte `InetAddress`). `mi_tpi_*`: 102, 123, 124, 127, 130, 132, 134 with SVR4 layouts.
- Options: Mentat `optcom_req` uses the XTI 16-byte option header, `T_CURRENT` 0x80 and statuses 0x100/0x200/0x400. Levels/names from the `epcf` resources of `tcp`, `udp`, `rawip`.
- `UNIX_error`, disconnect reasons: BSD numbers (tcp uses 48, 54, 60, 61; error acks 72 for ENOSR).
- Unconfirmed: the numbers 104, 106 (data), 125, 126, 128, 131, 133 (they fit the alphabetical scheme; data primitives are not in the switch), and OT's `T_INFO_ACK` values (the host's are passed, OT sets `TIDU_size` −1). `T_OPTDATA_REQ` is not supported.

## Mapping decisions

- Options: NoDelay, MaxSeg → `IPPROTO_TCP`; KeepAlive `{onoff, minutes}` → `SO_KEEPALIVE` (minutes kept locally); generic buffer/lowat/debug/linger and IP ReuseAddr/DontRoute/Broadcast → `SOL_SOCKET`; IP Options → `IPPROTO_IP` 1. TCP thresholds, OOBInline, UrgentPtr, UDP Checksum: local values. TOS, TTL, multicast, RcvDstAddr, HdrIncl, RxICMP: `T_NOTSUPPORT` (AMIX has only `IP_OPTIONS`).
- XTI `T_NEGOTIATE` → host `T_NEGOTIATE`; `T_CURRENT` and `T_CHECK` → host `T_CHECK` (the host's "get"; `T_CHECK` echoes the request); `T_DEFAULT` → host `T_DEFAULT`. Host error → `T_FAILURE`; overall status is the worst one.
- Options carried by `T_CONN_REQ`/`T_UNITDATA_REQ` are negotiated as sticky options before the primitive.
- Errors: host TLI 1–19 unchanged, others `TSYSERR`; SVR4 errnos → BSD.

## Security

- Streams and sessions are opened by the Mac process: the host's own checks apply (ports < 1024: AMIX `tcpopen` records `suser()` of the opener, `in_pcbbind` tests it, so the service procedure's context does not matter).
- `OTX_ATTACH` and `OTB_SETRING` accept only the session's owner (process and pid). Minors other than 0 do not open. At most 8 sessions per user but root.
- `T_CONN_RES` accepts only onto a TCP endpoint of the same session; the host trusts the queue pointer otxti puts there.

## DLM extension (`dlm.diff`, `relink-mac.sh.diff`)

- `/dev/otbridge` uses the loader's character-driver linkage: `modadm(MOD_TY_CDEV, …)` registers `otbridge` at major 55 (the module does not load without it); the first open loads the module; each open holds it until its close.
- `dlm/dlm_str.c`: `fmodsw` enlarged by 16 rows (override of the stock table, copied from `__amix_fmodsw` at `dlm_init`); `mod_strops` appends or reclaims a row by name. A row points at the loader's own streamtab whose read qinit has open and close trampolines: a queue pair holds the module from the open that sets `q_ptr` to its close; a removed row fails opens with `ENXIO`.
- `<sys/moddefs.h>`: `struct mod_str_data`, `MOD_STR_WRAPPER`. `otbridge` writes its two-linkage wrapper by hand.
- `dlm/libmod/*.s`: the stubs set `errno` themselves (a jump to `_cerror` through the PLT would return the lazy binder's address in `%d0`).
- Deviations from `docs/dlm-impl-spec.md`: no `MOD_TY_STR` registration or auto-load on `I_PUSH` (loading through `/dev/otbridge` installs `otxti`), no host-harness coverage of the STREAMS linkage.
- `devtab.diff`: `checkimg.py` dumps the stock `fmodsw` rows; device table rows for major 55 and the enlarged `fmodsw`. `root.manifest.diff`: `/dev/otbridge`.

```sh
patch -p1 < kernel/otbridge/dlm.diff
patch -p1 < kernel/otbridge/relink-mac.sh.diff
patch -p1 < kernel/otbridge/devtab.diff
patch -p1 < kernel/otbridge/root.manifest.diff
sh kernel/build.sh
sh kernel/otbridge/build.sh -k kernel/build/unix-mac.elf
```

## Tests

- Host: `sh kernel/otbridge/build.sh` (43 checks).
- Stations: `n0n2/test/t_otbst.c` (network root, after `t_net`).
- Runtime: `tests/otb/t_otb.c` (in `runall` and the network root); `tests/otb/build.sh` builds the module against the kernel under test; `mktestroot.sh` installs it at `/tests/otb/otbridge` with `/dev/otbridge`. Skips without the module.

## Known limits

- Pages that the owner no longer maps when the ring is unregistered or the session closes (copy-on-write after a fork, the owner's exec or exit with the descriptor still open elsewhere) cannot be unlocked through its address space and stay locked (`st_ringleak`); after 64 such rings `OTB_SETRING` fails `EAGAIN`. Unlocking them needs an exit hook shared with `guestcore`.
- Stations: bridge mode only; anchor mode (DHCP answers, nothing on the wire), `otddp` and `rawip` are not implemented.

## Not yet done

- The `.ENET` driver (`n0n2/enet`) as a network sResource in the synthetic declaration ROM, with the Slot Manager's `sGetDriver`; signal-driven receive in place of the per-tick poll.
- Mac relay: resolves `AF_DNS` addresses and `OT_RESOLVEADDR_REQ` (142) and answers OT-private ioctls; turns `OTB_R_DATA` records into what OT's upper modules expect; reassembles `OTB_RF_MORE` records; treats `write` returning 0 or `EAGAIN` as would-block; re-registers the ring when the signal comes and `OTB_GETRING` says it is lost; registers `otbridge` at major 55 before opening `/dev/otbridge`; checks the unconfirmed primitive numbers above.
