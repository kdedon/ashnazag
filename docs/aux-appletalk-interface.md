# A/UX kernel AppleTalk interface used by the Mac environment

Analysis of A/UX 3.1 (`/unix`, `/mac/lib/Patches/Patch.067C`), plus the shipped headers `/usr/include/at/*.h` and man pages `appletalk(7)`, `appletalk(1M)`. The headers are authoritative for names and structures. This is the spec a host (ASV, AMIX) must provide so `Patch.067C`'s AppleTalk runs unchanged ("pass-through").

Addresses are link addresses. "Unconfirmed" marks inferences.

## Summary

- AppleTalk Phase 2 only. EtherTalk or LocalTalk, one interface at a time (`appletalk(7)`).
- The kernel implements LAP (EtherTalk/LocalTalk), AARP, DDP with routing, the RTMP/ZIP/NBP packet handlers, EP, **ATP** and **ADSP**, all as STREAMS drivers and modules. The Mac side (`Patch.067C`) implements the Mac driver shells (`.MPP`, `.ATP`, `.XPP`, `.DSP`), the NBP name table and lookups, ZIP queries, and ASP/AFP client code on top.
- Transport to the Mac side: a clone DDP stream per socket, `I_STR` ioctls, `read`/`write` of DDP datagrams, one system call (`atp_control`, 167), and **a 4 KB event ring in Mac memory that the kernel writes directly**, plus SIGIO.

## 1. Devices

| Path | Kernel | Opened by | Purpose |
|---|---|---|---|
| `/dev/appletalk/ddp/socket` | clone (major 12), minor = DDP driver major (17 in this kernel); `cdevsw[17]` → `ddpinfo` | `ddp_open` 0x3dfc8, `adsp_ddp_open` 0x3292e; `open(path, O_RDWR)` | one stream per DDP socket |
| `/dev/appletalk/lap/ethertalk0/control` | ELAP driver (`cdevsw[16]` → `elap_info`) | `elap_init` 0x3e3ba (built as `"%s/%s/%s"` from `/dev/appletalk/lap`, interface, `control`), `__at_changezone` 0x38fe2 | EtherTalk interface control |
| `/dev/appletalk/lap/localtalk0/control` | LLAP driver (`cdevsw[18]` → `llap_info`) | `llap_init1` 0x3e27a | LocalTalk control (not needed on Atari/Amiga hosts) |

Majors are assigned by `autoconfig` at boot; `/etc/init.d/{ddp,elap,llap}` recreate the nodes (`mknod ... c $CLONE_MAJOR $DDP_MAJOR`, `c $MAJOR_ID 0`). The nodes in the image show majors 16 for `localtalk0` and 18 for `ethertalk0`, the reverse of this kernel's `cdevsw` (presumably stale; irrelevant to a reimplementation).

Pushable STREAMS modules (`fmodsw`): `at_sig` (`at_sig_info`), `adsp` (`adspinfo`), `at_atp` (`atp_info`). Master files: `/etc/master.d/{ddp,elap,llap,at_atp,at_sig,adsp}`.

## 2. Stream set-up sequences

**Plain DDP socket** (`__at_openSkt_int` 0x39240, used by `.MPP` `POpenSkt`):
1. `ddp_open`: `open("/dev/appletalk/ddp/socket", O_RDWR)`; `ioctl(I_SRDOPT, RMSGD)` (`0x20005306`, arg 1); `I_STR DDP_IOC_BIND_SOCK` (socket byte; 0 = dynamic, kernel returns the assigned number; requested static sockets must be 1–127).
2. `I_STR DDP_IOC_RSTATUS_TABLE` (0xcb06) with the 4-byte pointer to the event table (`__pevent_table`).
3. `I_SRDOPT RMSGD` again; `FIOASYNC` on (`0x8004667e`); `I_PUSH "at_sig"` (`0x80095302`).
4. SIGIO (31) handler `at_sigio` registered.

**ATP socket** (`loc_atp_open` 0x378e2): as above, but after `DDP_IOC_RSTATUS_TABLE` it does `I_PUSH "at_atp"`, then `I_STR AT_ATP_SET_SOCKET_MAP` (0x7c16, 4 bytes = DDP socket number), then `FIOASYNC`, then `I_PUSH "at_sig"`. The stack is DDP driver ← `at_atp` ← `at_sig`.

**ADSP stream** (`adsp_ddp_open` 0x3292e, `adspInit`, `adspCLInit`): DDP socket (`I_SRDOPT`, `DDP_IOC_BIND_SOCK`), `I_PUSH "adsp"`, ADSP commands by `I_STR`; SIGIO (31) and SIGURG (30, `adsp_urgent`) handlers.

**.MPP open** (`__DDP_Open` 0x388f6): allocates the event table from the System heap (`NewPtrSys`), page-aligned, 0x1000 bytes, zeroed; `at_interface_up` (below); opens a DDP stream and registers the table with `DDP_IOC_LSTATUS_TABLE` (0xcb04). `__DDP_Close` sends `DDP_IOC_ULSTATUS_TABLE` (0xcb05) with `__event_table`.

`at_send_to_dev(fd, cmd, buf, &len)` (0x3e110) is the `I_STR` wrapper: `struct strioctl { ic_cmd = cmd; ic_timout = -1; ic_len = *len; ic_dp = buf }`, `ioctl(fd, I_STR = 0xc0105308)`; returns the updated length.

## 3. `I_STR` commands

Command = `(module_id << 8) | n`, module IDs `AT_MID(n) = 200 + n` (`atlog.h`): LLAP 0xc9, ELAP 0xca, DDP 0xcb, ZIP 0xd0, ADSP 0xd4, LAP 0xd6. ATP uses `('|' << 8) | n`.

| Cmd | Name (header) | Stream | Data | Used by |
|---|---|---|---|---|
| 0xcb01 | `DDP_IOC_GET_CFG` | DDP | out `at_ddp_cfg_t` {`u_short network_up`; `int flags` (`AT_IFF_LOCALTALK` 1, `AT_IFF_ETHERTALK` 2, `AT_IFF_DEFAULT` 0x40000); `at_inet_t node_addr`; `at_inet_t router_addr`; `int netlo, nethi`} | `ddp_config`, `__DDP_bridge_info`, `__DDP_netaddr_init`, `rtmp_netinfo`, `at_get_cur_lap` |
| 0xcb02 | `DDP_IOC_BIND_SOCK` | DDP | in/out 1 byte socket | `ddp_open`, `adsp_ddp_open` |
| 0xcb03 | `DDP_IOC_GET_STATS` | DDP | out `at_ddp_stats_t` | not used by the Mac side (`appletalk -s`) |
| 0xcb04 | `DDP_IOC_LSTATUS_TABLE` | DDP | in 4 bytes: event-table address | `__DDP_Open` |
| 0xcb05 | `DDP_IOC_ULSTATUS_TABLE` | DDP | in 4 bytes: event-table address | `__DDP_Close` |
| 0xcb06 | `DDP_IOC_RSTATUS_TABLE` | DDP | in 4 bytes: event-table address | every socket open |
| 0xca01 | `ELAP_IOC_GET_CFG` | ELAP control | out `at_elap_cfg_t` {`short network_up`; `atalk_addr node`; `atalk_addr initial_addr`; `at_nvestr_t zonename`; `char if_name[4]`} | `elap_init`, `elap_get_cfg` |
| 0xca03 | `ELAP_IOC_SET_CFG` | ELAP control | in `at_elap_cfg_t` (initial address hint from PRAM) | `elap_init` |
| 0xca04 | `ELAP_IOC_SET_ZONE` | ELAP control | in `at_nvestr_t` zone (hint from PRAM) | `elap_init` |
| 0xca05 | `ELAP_IOC_SWITCHZONE` | ELAP control | in `at_nvestr_t` zone | `__at_changezone` (then PRAM updated via `cWriteXPRam`) |
| 0xd601 | `LAP_IOC_ONLINE` | LAP control | — | `elap_init`, `llap_init1` |
| 0xd602 | `LAP_IOC_OFFLINE` | LAP control | — | `lap_shutdown` |
| 0xc901/0xc903 | `LLAP_IOC_GET_CFG/SET_CFG` | LLAP control | `at_llap_cfg_t` | `llap_init1` (LocalTalk only) |
| 0xd001 | `ZIP_IOC_GET_CFG` | DDP | out `at_zip_cfg_t` {`at_nvestr_t zonename`} | `zip_getmyzone` (fallback after an ATP GetMyZone to the router) |
| 0x7c16 | `AT_ATP_SET_SOCKET_MAP` | `at_atp` | in 4 bytes: DDP socket | `loc_atp_open` |
| 0x7c03 | `AT_ATP_ISSUE_REQUEST_DEF` | `at_atp` | request block (`atp_set_default` + ATP header + data) | `at_send_request` (ZIP queries) |
| 0x7c01 | `AT_ATP_CANCEL_REQUEST` | `at_atp` | TID | `__at_killSendReq`, `__at_relTCB` |
| 0x7c0d | `AT_ATP_RELEASE_RESPONSE` | `at_atp` | | `__at_relRspCB`, `atp_release_response` |
| 0x7c0e / 0x7c0f | `AT_ATP_SEND_RESPONSE` / `_EOF` | `at_atp` | | `__at_addResponse` |
| 0xd4f1–0xd4fd | `ADSPNEWCID` (241), `ADSPRESET` (242), `ADSPOPTIONS` (243), `ADSPATTENTION` (244), `ADSPREAD` (246), `ADSPSTATUS` (247), `ADSPCLDENY` (248), `ADSPCLLISTEN` (249), `ADSPCLOSE` (252), `ADSPOPEN` (253) | `adsp` | ADSP control blocks (`adsp.h`, `adsp_cb.h`) | `adspOpen`, `adspRead`, … (`adsp_ioctl.h` also defines `ADSPCLINIT` 251, `ADSPCLREMOVE` 250, `ADSPWRITE` 245, `ADSPATTNREAD` 254; not seen as immediates) |

Kernel dispatch: `ddp_wput` (switch on `cmd - 0xcb01`, 6 cases), `elap_wputq` (0xca01–0xca05, 0xd601/0xd602), `m_ioctl` (LLAP), `zip_ioctl`, `atp_wsrvc`.

The kernel handles the table registrations with `realvtop` (0x5a982), i.e. it records the **physical** address of the user table and writes through it; the table must stay resident and physically contiguous (4 KB, page-aligned). How A/UX guarantees residency of that System-heap page (shm locking or `UI_vm_holdmem`) is unconfirmed.

## 4. `atp_control` (system call 167)

`atp_control(op, ddp_socket, buf, arg)` via `trap #0` (stub 0x4038c). Kernel `atp_control` (0x1002cd00) returns EINVAL unless the process has the `SMAC` flag, then calls `atp_dispatch` = `atp_control1` (set by `atp_init`), which looks up `atp_socket_map[ddp_socket]` (filled by `AT_ATP_SET_SOCKET_MAP`) and switches on `op` (0–3):

| op | Name (`at_atp.h`) | Caller | args |
|---|---|---|---|
| 0 | `ATP_SENDREQUEST` | `__at_sendRequest`, `__at_sendNRequest` | buf = request, arg = length |
| 1 | `ATP_GETRESPONSE` | `atp_getResponse` | buf, arg = pointer to result |
| 2 | `ATP_SENDRESPONSE` | `__at_sendResponse` | buf = response set, arg = length |
| 3 | `ATP_GETREQUEST` | `atp_getRequest` | buf, arg = 0 |

Buffer layouts follow `at_atp.h` (`at_atpreq`, `atp_result`, `atpBDS`) and `atp_cb.h`.

## 5. Data and notification

- **Read option**: every DDP stream is set to message-discard mode (`I_SRDOPT RMSGD`); one `read` returns one datagram.
- **Send**: `__at_writeDDP` gathers the MPP write-data structure and calls `write(fd, buf, len)` starting **after** the 3-byte LAP header, i.e. an extended DDP header (13 bytes, `at_ddp_t` in `ddp.h`) followed by data (max 586). The DDP type byte is filled from the socket entry.
- **Event ring** (`struct at_events`, `atp_events.h`, 0x1000 bytes): `e_rindx`, `e_windx`, `e_rdindx`, `e_wdindx`; 520 FIFO entries `{u_char e_socket, e_type; u_short e_length}` at +0x10; 50 data slots of 40 bytes at +0x830. Kernel writer: `atp_queue_event` (0x10059ef6). Event types: `E_TYPE_SNDREQ` 0, `E_TYPE_GETREQ` 2, `E_TYPE_GETREQM` 3 (with inline data), `E_TYPE_DDPPKT` 4, `E_TYPE_DDPPKTM` 5 (with inline data). The kernel sends **SIGIO (31)** to the owning process when an event is queued into an empty ring.
- **Receive** (`at_sigio` 0x38436): drains the ring; type 4 → `read(fd, buf, 600)`; type 5 → copy from the 40-byte data slot; checks for its own packets unless `sendtoself`; passes datagrams to `__DDP_rcv` (the Mac socket listener). Types 0–3 go to `atp_getResponse`, `atp_getRequest`, `atp_getRequestM`, `atp_release_response`.
- **ADSP**: completion via SIGIO; attention/urgent via **SIGURG (30)**.
- `select` is not used on AppleTalk streams by the Mac side.

## 6. Protocol split

| Layer | Kernel (`/unix`) | Mac side (`Patch.067C`) |
|---|---|---|
| LAP | ELAP (`elap_*`, EtherTalk Phase 2 over the Ethernet driver), LLAP (`llap_*`, `lap_iop*` via the SCC IOP) | interface selection (`at_interface_up`, `lap_init`) |
| AARP | `aarp_init`, `aarp_send_data` | — |
| DDP | `ddp_*`: sockets, checksums, best-router table (`ddp_brt_init`, `ddp_age_router`), gleaning, `ddp_input/output` | `.MPP` shell, socket listeners, `__at_writeDDP`, `__DDP_rcv` |
| RTMP | `rtmp_init`, `rtmp_handler` (router tracking) | `rtmp_netinfo` |
| ZIP | `zip_init`, `zip_handler`, `zip_control`, `zip_sched_getnetinfo`, `zip_handle_getmyzone` | `zip_getmyzone`, `zip_getlocalzones` (ATP to the router; `ZIP_IOC_GET_CFG` fallback) |
| NBP | `nbp_init`, `nbp_handler`; `nbp_input` dispatches DDP types 1/2/6 to the RTMP/NBP/ZIP handlers | name table, register/lookup/confirm (`__at_registerName`, `__at_lookup`, `nbp_*`); how NBP lookups for Mac-registered names reach the Mac side is unconfirmed |
| EP | `ep_init` (echo responder) | — |
| ATP | full protocol (`atp_*`: transactions, retries, XO, release timers) | `.ATP` shell over `atp_control` and `at_atp` ioctls |
| ADSP | full protocol (`adsp*` module) | `.DSP` shell (`adspDriver`, `ADSPControlTable`) |
| ASP/AFP (`.XPP`), PAP | — | user space (`.XPP` over ATP; PAP in printer drivers) |

## 7. Configuration and start-up

- `/etc/sysinitrc`: if `/dev/appletalk/ddp/socket` exists, loads IOP code and runs `/etc/appletalk -cu` (bring up only if PRAM says active); otherwise `appletalk -d`.
- `/etc/appletalk` (`appletalk(1M)`): `-u` up and save to PRAM, `-d` down, `-c` obey PRAM, `-i ethertalk0|localtalk0`, `-b ae0…` (Ethernet hardware), `-z` ignore the zone hint, `-n` node, `-p` PRAM settings, `-s` statistics. `appletalkrc` is obsolete.
- PRAM holds the active state, the interface, the zone hint and the node hint. The Mac side reads it (`at_interface_up` reads XPRAM 0x400e0; `elap_init` uses `readxpram`) and writes it on zone change. The Network control panel switches interface; the Chooser toggles AppleTalk.
- Mac-side bring-up (`at_interface_up` 0x39440): `ddp_config`; picks `ethertalk0` or `localtalk0` from PRAM and the slot board (`slot_board_id`, `slot_driver_name`); `lap_init` → `elap_init` (open control, `ELAP_IOC_SET_CFG` with the PRAM node hint, `ELAP_IOC_SET_ZONE` with the PRAM zone, `LAP_IOC_ONLINE`) or `llap_init1`.

## 8. Minimum host implementation (EtherTalk only)

1. `/dev/appletalk/ddp/socket` as a clone STREAMS device: `I_SRDOPT RMSGD`, `FIOASYNC`/SIGIO, one datagram per `read`, `write` of an extended DDP datagram.
2. `/dev/appletalk/lap/ethertalk0/control` with `ELAP_IOC_GET_CFG/SET_CFG/SET_ZONE/SWITCHZONE`, `LAP_IOC_ONLINE/OFFLINE`.
3. DDP `I_STR`: `GET_CFG`, `BIND_SOCK`, `LSTATUS_TABLE`, `ULSTATUS_TABLE`, `RSTATUS_TABLE`; ZIP `ZIP_IOC_GET_CFG`.
4. The event-ring protocol: accept the table address, write `at_events` entries into the Mac process's memory (on the host this can be a copy into the process's address space instead of a physical write), SIGIO on empty-to-non-empty.
5. Pushable modules `at_sig`, `at_atp` (with `AT_ATP_SET_SOCKET_MAP`, `ISSUE_REQUEST_DEF`, `CANCEL_REQUEST`, `RELEASE_RESPONSE`, `SEND_RESPONSE[_EOF]`), `adsp` (the `0xd4xx` commands above, SIGURG for attention).
6. System call 167 `atp_control` (ops 0–3), restricted to Mac processes, keyed by DDP socket.
7. Protocol engines: EtherTalk Phase 2 framing (802.2 SNAP, AppleTalk 0x809B / AARP 0x80F3) over the host DLPI Ethernet driver, AARP, DDP with best-router table, RTMP/ZIP start-up (network range, zone), NBP handling on socket 2, EP, ATP, ADSP.
8. An `appletalk`-equivalent to bring the interface up and keep state in PRAM (the Mac side also does this itself through the ELAP control device).
9. Not needed on Atari/Amiga: LocalTalk (`llap`, IOP code).

## 9. Kernel imports of the converted modules

`ddp`, `elap`, `at_atp`, `adsp` and `at_sig` from `/etc/boot.d` converted with `tools/coff2elf` and combined with `m68k-elf-ld -r` link cleanly; `objdump -dr` shows correct `R_68K_32` relocations. The combined object imports 48 kernel symbols, which is the shim's contract:

| Group | Symbols | On AMIX |
|---|---|---|
| STREAMS DDI | `allocb` `allocq` `canput` `copyb` `copymsg` `dupb` `dupmsg` `flushq` `freeb` `freemsg` `getq` `linkb` `unlinkb` `msgdsize` `putbq` `putq` `qenable` `qreply` | present (same SVR STREAMS; check `mblk_t`/`queue_t` layouts, which differ) |
| BSD mbufs | `allocmb` `mclgetx` `mclput` `m_freem` | absent: shim (A/UX-specific mbuf path, used by `elap`) |
| Kernel services | `bcmp` `blt` `bzero` `copyin` `copyout` `suword` `useracc` `sleep` `wakeup` `psignal` `timeout` `untimeout` `panic` `printf` `nulldev` `lbolt` | present or trivial wrappers (`blt` = A/UX block copy) |
| Priority | `spl6` `splclock` `splimp` `splx` | map to AMIX spl levels |
| Machine | `realvtop` `undma` `probing` | shim |
| Global structures | `u` `v` | **direct field access**: A/UX `struct user`/`struct var` offsets differ from AMIX, so these accesses need binary patching or a shadow structure |
| Cross-module | `atp_dispatch` | defined elsewhere in A/UX (system call 167 path, §4); provided by our personality |

## Open points

- How NBP lookup packets for names registered by the Mac side are delivered (kernel `nbp_handler` vs. Mac socket 2).
- Exact layouts of the ATP request/response buffers passed to `atp_control` and `AT_ATP_ISSUE_REQUEST_DEF` (follow `at_atp.h`; not byte-checked).
- How A/UX keeps the event-table page resident for `realvtop`.
