# Open Transport pass-through for Mac OS 8.1

Network pass-through for Mac OS 8.1 (Open Transport 1.3) in the A/UX-style Mac environment on the unified SVR4 kernel: Mac programs share the Unix host's network identity. TCP/UDP endpoints become host TLI endpoints (AMIX `/dev/tcp`, `/dev/udp`); AppleTalk endpoints become host DDP sockets (A/UX kernel modules). A virtual Ethernet port comes first, as bring-up milestone, fallback and anchor for OT's own IP layer.

Status: design. Nothing here is built.

Inputs: the 8.1 and 7.6.1 CDs (user-supplied, inspected read-only; §1), *Inside Macintosh: Networking With Open Transport* (rev. 1.3, 1997), Apple Q&A NW19, [aux-syscall-translation.md](aux-syscall-translation.md) §7, [aux-kernel-design.md](aux-kernel-design.md) §7–§8, [aux-appletalk-interface.md](aux-appletalk-interface.md), [aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md), [aux-uinter-semantics.md](aux-uinter-semantics.md), [dlm-impl-spec.md](dlm-impl-spec.md), [guest-container-design.md](guest-container-design.md), [display-service-design.md](display-service-design.md), [macos8-requirements.md](macos8-requirements.md), `kernel/mac/sonic/NOTES.md`. *(verify)* marks a fact to check before implementing.

## 0. Summary of decisions

| Topic | Decision |
|---|---|
| Target architecture | **(c) hybrid, ending in full pass-through**: TCP/UDP by an XTI-level relay to host TLI endpoints; AppleTalk by a DDP-level relay to the host's A/UX AppleTalk modules; a virtual Ethernet port for bring-up, fallback and as the inert anchor for OT's `ip` |
| Replacement point | OT's own STREAMS modules by ASLM class ID: `OTModl$tcp`, `OTModl$udp`, `OTModl$rawip` (TCP/IP), `OTModl$ddp`, `OTModl$nbp` (AppleTalk). OT's client libraries, `dnr`, `atp`, `adsp`, `zip`, `pap` and control panels stay Apple's |
| Mac-side modules | Thin relays: one host stream per OT endpoint, TPI messages shipped as records over `read`/`write`; no protocol logic |
| Kernel side | DLM `otbridge`: STREAMS module `otxti` (OT TPI dialect ↔ AMIX TPI), `otddp` (OT DDP ↔ A/UX DDP stream), control clone device `/dev/otbridge` (event ring, sessions, virtual Ethernet stations) |
| Mac → host calls | A/UX Unix calls through `_AUXSysCall` (`$ABFA`), as `Patch.067C` makes them: `open`, `read`, `write`, `ioctl` (`I_PUSH`, `I_STR`, `I_SRDOPT`), `close`. No new system calls |
| Host → Mac events | A 4 KB event ring in Mac memory written by the kernel plus SIGIO on empty → non-empty (the A/UX AppleTalk mechanism); the Mac routine is registered with `_AUXDispatch` selector 23 and drains at deferred-task time |
| Virtual port | Classic `.ENET` driver in the synthetic declaration ROM of a fake NuBus network card; OT's `enetDRVR` wraps it. Host side: a virtual station on the host NIC (own MAC in the SONIC CAM, local switching) |
| Security | Every host stream is opened by the Mac process with its user's credentials: host rights exactly, ports < 1024 only for root, raw IP only if the host allows it and the container grants `net.raw`; the virtual port needs the `net.station` grant |
| Mac toolchain | 68k OT plug-ins must be ASLM libraries (Apple Q&A NW19). Primary: gcc (Retro68 or ours) + our `mkaslm` post-linker; cross-check: MPW 3.5 + ASLM and OT Module SDKs (user-supplied). `.ENET` driver: m68k `as` |
| Order | N1 host bridge + native harness; N2 virtual port; N3 TCP/UDP relay; N4 AppleTalk relay; N5 DNS and configuration |

## 1. Open Transport on 68k

### 1.1 What 8.1 installs

From the 8.1 CD (`System Folder:Extensions`, `System`), OT 1.3:

| File / resource | Type/creator | Content |
|---|---|---|
| Open Transport Library | `libr`/`OMGR` | core: `OTLib$mps` (Mentat Portable Streams, 157 KB), `kUtils`, `DLPISupportLib`, `TPI8022`, `NBLinks` (`.ENET`/`.TOKN`/`.FDDI` wrappers: `OTModl$enetDRVR`…), port scanners (`OTLib$pScan`, `cScan`, `NBPScnr` NuBus, `SrlABPScnr`), serial (`SerlAB`), `sad`, `log`, `echo`, `nuls`, `pipemod`, `tilisten`, client utilities |
| Open Tpt Internet Library | `libr`/`OMGR` | `OTLib$ip` (one 157 KB segment exporting `OTModl$ip`, `tcp`, `udp`, `rawip`, `arp` and `…m` variants), `OTLib$DNRMod` (`OTModl$dnr`, `dnr2`), `macip`, `bootp`, `dhcp`, `rarp`, `mdev`, Inet client and configurator libraries |
| Open Tpt AppleTalk Library | `libr`/`OMGR` | `OTLib$ddp` (exports `OTModl$ddp` and `OTModl$nbp`), `atp`, `adsp`, `zip`, `pap`, `adev802x`, `adevShim` (classic `atlk`/`adev` link drivers), `MPPCompat`/`MPPNative` (classic `.MPP` API), `LTPScnr` |
| OpenTransportLib, OpenTptInternetLib, OpenTptAppleTalkLib | `shlb`/`otan` | PowerPC CFM counterparts (unused on 68k) |
| Shared Library Manager | `INIT`/`OMGR` 2.0.1 | ASLM, OT's 68k runtime |
| Apple Built-In Ethernet, Apple Ethernet CS/LC/NB | `comd`/… | classic `.ENET` driver code in `enet` resources, keyed by Gestalt machine ID (43…94; none for the Quadra 800, whose driver is in ROM) |
| EtherTalk Phase 2 | `adev`/`et20` | classic AppleTalk link (`atlk`/`adev`), used through `adevShim` |
| AppleTalk control panel, AppleTalk Preferences | `cdev`/`atdv`, `pref`/`atdv` | AppleTalk port choice |
| `System` | | `DRVR` 127 `.ENET`, 9 `.MPP`, 10 `.ATP`, 40 `.XPP`, 126 `.DSP`; `otdr`/`otlm` 9 (OT `.MPP` stub, LAP Manager 60.3) |

The TCP/IP control panel and its `TCP/IP Preferences` come from the installer (not in the CD's System Folder). The 7.6.1 CD installs OT 1.1.x from its installers and carries the CFM-68K Runtime Enabler.

**Packaging.** Each ASLM library resource names a code-resource type (`cd70`, …: segments 0, `%A5Init`, `Main`), a version and the class IDs it exports and imports. Name spaces: `OTModl$<module>` (STREAMS modules and drivers, the names used in configuration strings), `OTLib$…`, `OTClnt$…`, `ot:pScnr$…` (port scanners), `OTKrnl$…`, `OTPortCfg$…`. A module exports `GetOT<name>InstallInfo` (seen: `GetOTenetDRVRInstallInfo`) returning its `install_info` (streamtab, flags: driver/module, upper/lower interface TPI or DLPI).

**Endpoint configurations** (`epcf` resources) give each provider's upper/lower interface and option table. The option numbers are OT's XTI numbering and drive §3.3:

| Provider | Interfaces | Options (level/name) |
|---|---|---|
| `tcp` | TPI / TPI | NoDelay 6/1, MaxSeg 6/2, KeepAlive 6/8 (8 bytes), Notify/Abort/ConnNotify/ConnAbort thresholds 6/0x10–0x13, OOBInline 6/0x14, UrgentPtr 6/0x15, SndBuf/RcvBuf 0xffff/0x1001/0x1002, SndLoWat/RcvLoWat 0xffff/0x1003/0x1004, Debug 0xffff/1 |
| `udp` | TPI / TPI | Checksum 17/0x600, RxICMP 17/2 |
| `rawip` | TPI / DLPI | Options 0/1, TOS 0/2, TTL 0/3, ReuseAddr 0/4, RcvOpts 0/5, DontRoute 0/0x10, Broadcast 0/0x20, HdrIncl 0/0x1002, RcvDestAddr 0/0x1007, multicast 0/0x1010–0x1015, DVMRP 0/0x64–0x6b |
| `ddp` | TPI / DLPI | Checksum |
| `atp`, `adsp`, `pap` | TPI / TPI | Checksum and per-protocol options |

So `tcp`/`udp` sit on `ip` through a TPI boundary, `ddp` sits directly on the port's DLPI, and `atp`/`adsp`/`pap` sit on `ddp` through TPI. Those boundaries are where the relays go.

### 1.2 Ports

- A port is a link device OT knows by a port reference (bus, device type, slot, other) and a name ("Ethernet slot D").
- Port scanners run at OT load and register ports. 68k: the NuBus scanner finds Ethernet cards through the Slot Manager; built-in Ethernet comes from the ROM `.ENET` or an `enet` resource.
- Classic `.ENET` drivers become DLPI ports through `enetDRVR` (`TLinkDRVRGrp::OpenSlotDriver`, attach/detach protocol handlers, multicast, `GetPhysicalAddress`, SNAP demux for 802.2). A classic `.ENET` driver is therefore a complete OT Ethernet port on 68k. *(verify: the NuBus scanner's sResource match — category network, type Ethernet — against a synthetic card)*

### 1.3 Execution model

- Levels: hardware interrupt, deferred task, system task. Modules' put and service routines run at deferred-task time; OT client notifiers normally too.
- Endpoints are synchronous or asynchronous; synchronous mode is built by OT over the same asynchronous providers, so providers never block.
- 68k OT depends on ASLM, the Deferred Task Manager, the Time Manager and the Memory Manager (see [macos8-requirements.md](macos8-requirements.md)).
- In our environment all Mac "interrupts" are Unix signals; `Patch.067C`'s `signal_handler` runs registered routines and then `doDTasks` ([aux-interrupts-and-gateways.md](aux-interrupts-and-gateways.md) §1). A relay's SIGIO routine schedules an OT deferred task and returns.

### 1.4 Public documentation and licences

| Source | Content | Status |
|---|---|---|
| *Inside Macintosh: Networking With Open Transport* 1.3 (Apple, PDF on developer.apple.com archive) | client API, options, addresses, ports, execution levels | cite only (copyright, backup-copy clause) |
| Apple Q&A NW19 "Open Transport and CFM-68K" | 68k plug-ins only as ASLM; CFM-68K plug-ins unsupported; CFM-68K clients from OT 1.3 | cite |
| OT Protocol/Module SDK, "Open Transport Module Developer Note", "STREAMS Modules and Drivers", "Open Tpt CFM68K Dev. Note" | module/driver interfaces, `install_info`, port registration, Mentat `mi_*` helpers | not public now; user-supplied from old developer CDs |
| Universal Interfaces 3.x (`OpenTransport.h`, `OpenTptInternet.h`, `OpenTptAppleTalk.h`, `OpenTransportProtocol.h`, `OpenTransportKernel.h`) | constants and structures (TPI primitive numbers, `install_info`, port records) | Apple licence forbids redistribution; user-supplied (MPW 3.5) |
| ASLM Developer's Guide | library format and build | user-supplied |
| Retro68 (GPL-3.0+) | 68k/PPC classic cross compiler, Rez; Multiversal Interfaces (free) lack OT and ASLM | open source |

Nothing clearly redistributable was found, so `ref/ot/` stays empty; the CD facts above were read from the user's media.

### 1.5 Precedents

- **A/UX 3.x** ships System 7.0.1 with classic networking only: `Patch.067C`'s MacTCP (`.ipp`) over Unix sockets and its `.MPP`/`.ATP`/`.XPP`/`.DSP` over the kernel's AppleTalk streams. It never had OT.
- **Basilisk II** (GPL): a patched `.ENET` driver; host back ends slirp (user-mode NAT, DHCP, no inbound), TAP/ethertap, `sheep_net`; the Mac gets its own IP. **SheepShaver** does the same for PowerPC with an OT DLPI driver. Both are option (b); slirp is option (d) below.

## 2. Architecture options

```
 Mac OS 8.1 (one A/UX Mac process, uid of the user)
   OT client libs ── tcp ─┐  udp  rawip │ dnr │ atp adsp pap zip
                    (a) relay modules  │     │   └─ on ddp ─┐
                          │            ip    │        (a) ddp/nbp relay
                          │             └ enetDRVR ── .ENET (virtual card)  (b)
 ─────────────────────────│──────────────────────────────────│──────── A/UX calls
   kernel: fd per endpoint + otxti          otbridge station   fd + otddp
           AMIX /dev/tcp /dev/udp            SONIC CAM/switch   /dev/appletalk/ddp/socket
```

| | (a) XTI/DDP relay | (b) virtual Ethernet port | (c) hybrid (recommended) | (d) port + host NAT (slirp-style) |
|---|---|---|---|---|
| Identity | host's IP and AppleTalk node | own MAC, IP, node | host's (TCP/UDP, AppleTalk); port only as anchor | host IP outbound; inbound only by forwarding table |
| Mac-side code | ASLM relay library | `.ENET` driver in fake ROM | both | `.ENET` driver |
| Host code | `otxti`, `otddp`, ring | station + local switch | all of (a) and (b) | user-space TCP/IP terminator (~10k lines) |
| Mac servers | host ports, host rules | own address | host ports | configured forwards only |
| Double stack | no | no | no | yes (OT TCP + terminator TCP) |
| Main risk | ASLM replacement mechanics, `mkaslm` | NIC multi-station support | as (a) | performance, inbound semantics |

**Recommendation: (c).** (a) matches the pass-through decision and A/UX's own model; (b) is small, needs no ASLM, and gives a working network early; OT's `ip` still needs a port to bind even when `tcp`/`udp` are relays, and the (b) port in anchor mode is that port (§3.6). (d) is the fallback if ASLM replacement proves impossible (§7).

## 3. Kernel side: DLM `otbridge`

### 3.1 Pieces

| Piece | Linkage | Content |
|---|---|---|
| `otxti` | `MOD_STR_WRAPPER` | pushed on AMIX `/dev/tcp`, `/dev/udp`, `/dev/rawip` streams; record framing, TPI dialect and option translation, address bookkeeping, event posting |
| `otddp` | `MOD_STR_WRAPPER` | pushed on `/dev/appletalk/ddp/socket` streams (N4); OT DDP TPI ↔ A/UX DDP datagrams and `I_STR` |
| `/dev/otbridge` | `MOD_DRV_WRAPPER`, clone | minor 0 session control (event ring, policy, host info); minor 1 virtual Ethernet station (N2) |
| sn station support | static, `kernel/mac/sonic` | secondary unicast CAM entries and local switching between the host station and virtual stations |

`$depend`: none for N1–N3 (AMIX inet is static); the AppleTalk DLM for `otddp`. Written K&R, builds on the host with `-DOTB_HOST` for the translation harness, as the SONIC driver does.

### 3.2 Endpoint streams and records

- One host stream per OT endpoint, opened by the Mac process: `open("/dev/tcp", O_RDWR|O_NDELAY)`, `I_PUSH "otxti"`, `I_STR OTX_ATTACH {session, cookie}`. The descriptor is real: the endpoint's lifetime, `fork`, close-on-exit and resource limits are ordinary Unix ones.
- **Records**: both directions carry one TPI message per record: `{u_char type (M_PROTO, M_PCPROTO, M_DATA); u_char flags; u_short ctllen; u_long datalen; ctl; data}`. Down: one `write` per record; `otxti` rebuilds M_PROTO + M_DATA. Up: `otxti` turns each upstream message into one M_DATA record; the stream is in `RMSGN` mode so one `read` returns one record; data longer than the Mac's record buffer (8 KB) is split with `T_MORE`.
- Only calls A/UX already has cross the boundary, so the personality needs no new translation and the native harness (N1) uses the same records.
- Rejected: one multiplexed session stream with kernel-held lower transport streams. It saves descriptors and syscalls but needs file-less stream opens and loses the per-endpoint Unix semantics. Revisit if descriptor limits bite (§7).

### 3.3 TPI dialect translation (`otxti`)

OT speaks XTI-era Mentat TPI; AMIX's inet speaks SVR4.0 TPI as used by `timod`/`sockmod`.

| OT → host | Translation |
|---|---|
| `T_BIND_REQ`, `T_UNBIND_REQ`, `T_CONN_REQ`, `T_CONN_RES`, `T_DISCON_REQ`, `T_DATA_REQ`, `T_EXDATA_REQ`, `T_ORDREL_REQ`, `T_UNITDATA_REQ` | pass through; addresses translated (OT `InetAddress` {`fAddressType` 2, port, host, 8 zero bytes} = AMIX `sockaddr_in` layout, checked field by field); options in `T_CONN_REQ`/`T_UNITDATA_REQ` translated as below |
| `T_CONN_RES` acceptor | OT names the acceptor by its endpoint; the host needs the acceptor stream's read queue. The Mac passes the acceptor's cookie; `otxti` finds that stream in the session and fills `QUEUE_ptr` (the `I_FDINSERT` equivalent of [aux-syscall-translation.md](aux-syscall-translation.md) §7.3 accept) |
| `T_OPTMGMT_REQ` (XTI `TOption` list, `T_NEGOTIATE`/`T_CHECK`/`T_DEFAULT`/`T_CURRENT`) | per option: map to TLI `opthdr` (below), send one host `T_OPTMGMT_REQ`, rebuild the XTI reply with per-option status; unknown → `T_NOTSUPPORT` |
| `T_ADDR_REQ` | answered by `otxti` from addresses it recorded (bind ack, connect, connect confirm/indication), as `timod` does for `TI_GETMYNAME` |
| `T_OPTDATA_REQ` | data with options: options negotiated first, then `T_DATA_REQ`/`T_EXDATA_REQ` |
| `T_INFO_REQ` | host `T_INFO_ACK`, `SERV_type` and sizes adjusted to what OT's `tcp`/`udp` report *(verify against OT's own values)* |
| OT-private `M_IOCTL`s (`I_OT…`) | answered on the Mac side by the relay; never reach the host |

| OT option | Host option |
|---|---|
| `INET_TCP` NoDelay, MaxSeg | `IPPROTO_TCP` `TCP_NODELAY`, `TCP_MAXSEG` |
| `INET_TCP` KeepAlive `{onoff, timeout}` | `SOL_SOCKET` `SO_KEEPALIVE` (timeout is host-wide) |
| `INET_TCP` thresholds, UrgentPtr, OOBInline | local (host TCP keeps its own timers; OOB per `T_EXDATA`) *(verify OOBInline)* |
| `XTI_GENERIC` SndBuf, RcvBuf, SndLoWat, RcvLoWat, Debug, Linger | `SOL_SOCKET` same numbers |
| `INET_IP` ReuseAddr 4, DontRoute 0x10, Broadcast 0x20 | `SOL_SOCKET` `SO_REUSEADDR`, `SO_DONTROUTE`, `SO_BROADCAST` (same numbers, other level) |
| `INET_IP` TOS, TTL, Options, multicast | `IPPROTO_IP` equivalents where AMIX has them *(verify AMIX 2.1 IP options and multicast)*; otherwise `T_NOTSUPPORT` |
| `INET_UDP` Checksum, RxICMP | local / `T_NOTSUPPORT` |

Errors: host `T_ERROR_ACK` TLI errors map to OT's `kOT…Err` (`TACCES` → `kOTAccessErr` for privileged ports), `T_DISCON_IND` reasons pass through (ECONNREFUSED, ETIMEDOUT as OT expects *(verify reason encoding)*).

### 3.4 Events

- `/dev/otbridge` minor 0 is the session: the Mac opens it once, registers a 4 KB page in its heap with `OTB_SETRING` and a signal (default SIGPOLL/AMIX 22 = A/UX SIGIO 31). The kernel locks the page and writes it with `aux_upoke`, as for the AppleTalk ring ([aux-kernel-design.md](aux-kernel-design.md) §8).
- Ring entry `{u_long cookie; u_short events}`; events: readable, writable (back-enabled after a full queue), hang-up, error. One pending bit per endpoint coalesces events, so the ring holds at most one entry per endpoint and cannot overflow (512 entries).
- `otxti`'s read put procedure queues the record and posts "readable" on the first message; the stream's write side posts "writable" when back-enabled.
- Signal on empty → non-empty only; the Mac routine (registered by `_AUXDispatch` 23, `register_signal(31, …)`) schedules one OT deferred task, which drains the ring. Early bring-up can skip the ring and `select` over endpoint descriptors.

### 3.5 Flow control

- Down: all Mac descriptors are `O_NDELAY`. `write` returning EAGAIN leaves the OT message on the relay's write queue (`putbq`, `noenable`); the "writable" event re-enables it.
- Up: the relay drains an endpoint only while `canputnext` holds upstream; otherwise it stops, data stays in the host stream head, and the host TCP window closes.
- No Mac call blocks: a blocking Unix call from Mac code stops every Mac task in the session.

### 3.6 Virtual Ethernet port (station backend)

- `/dev/otbridge` minor 1 = one station: a DLPI-style raw frame stream with its own MAC (locally administered, derived from host MAC and uid, or configured). `OTB_STATION {mac, mode}`.
- **sn support**: a station is a secondary CAM entry (SONIC has 16; the host station and multicast share the rest); frames to it go to its stream; frames between host station and virtual stations, and broadcasts/multicasts, are copied locally by `sndlpi`, because the chip does not receive its own transmissions. A station may only send with its own source MAC; no promiscuous mode.
- Other hosts' NICs (A2065, ASV `la`): the same switching in a generic STREAMS `vsw` layer under `ip`/`arp`, with the NIC in promiscuous mode *(later)*.
- **Modes**: `bridge` (N2: the Mac's own IP/AppleTalk on the LAN) and `anchor` (N3+: nothing reaches the wire; the kernel answers the Mac's DHCP requests with the host's address, mask, router, DNS servers and domain, and ARP for the router; everything else is dropped). In anchor mode OT's `ip` carries the host's address, so `OTInetGetInterfaceInfo` and the TCP/IP control panel show the host's identity without writing private preference formats.
- Mac side: a classic `.ENET` `DRVR` (attach/detach protocol handler, write, multicast, get info) in the fake network card's declaration ROM; the kernel Slot Manager ([display-service-design.md](display-service-design.md) §4) already serves the fake video card. Receive runs from the same SIGIO/deferred-task drain; protocol handlers get `ReadPacket`/`ReadRest` over the record buffer. OT's NuBus scanner and `enetDRVR` make it "Ethernet slot D"; classic 7.x software sees the same `.ENET`.
- On Quadra hosts the built-in SONIC belongs to the host kernel; the emulated Slot Manager does not expose the ROM's built-in Ethernet to the Mac.

### 3.7 Security

- Streams are opened in the Mac process with its credentials; AMIX `tcp`/`udp` apply their own checks (ports < 1024 need root *(verify where AMIX checks: open-time `cred` or bind)*). A Mac program can do exactly what a Unix program of that user can.
- `rawip` relays open `/dev/rawip`/`/dev/icmp`, which the host restricts; the container must also grant `net.raw` ([guest-container-design.md](guest-container-design.md) §4.6).
- Stations need `net.station` (root-granted per container), give a new LAN identity and are limited to their own MAC.
- `OTX_ATTACH` checks that the session and the stream belong to the calling process.

### 3.8 Why not `guestcall` or `sockmod`

- The Mac profile reaches the host through Unix calls; the container's `GUEST_SOCK` service is the socket back end for guests without them (Amiga `bsdsocket`). `otxti` and the event ring can serve it later.
- `sockmod` implements socket semantics; OT wants TPI. Both paths use the same in-kernel TLI binding (open the transport device in the process, push a module), so A/UX sockets ([aux-syscall-translation.md](aux-syscall-translation.md) §7) and OT endpoints share the host stack, descriptors and rules.

## 4. Mac side

### 4.1 Components

| Component | Form | Stage |
|---|---|---|
| `.ENET` driver + network sResource | `DRVR` image in the synthetic declaration ROM (kernel data) | N2 |
| AUX Internet relay | ASLM library file in Extensions exporting `OTModl$tcp`, `OTModl$udp`, `OTModl$rawip` | N3 |
| AUX AppleTalk relay | ASLM library exporting `OTModl$ddp`, `OTModl$nbp` | N4 |
| Session code | shared ASLM library `AUXNet$session`: `/dev/otbridge` open, ring, SIGIO routine, deferred-task drain, cookie table | N3 |
| Classic network off | in the 8.1 System graft extension: `Patch.067C`'s `.ipp` (MacTCP) and `.MPP`/`.ATP`/`.XPP`/`.DSP` shells are not installed when OT is present, so one path exists per protocol | N2 |

### 4.2 Relay module

- Driver (`install_info` flags: driver, upper TPI, lower TPI) with the same name and option table as OT's module.
- `open`: open the host stream, push `otxti`, attach; allocate the endpoint state.
- `wput`/`wsrv`: OT-private ioctls answered locally; every TPI message → record → `write`.
- Drain (deferred task): `read` records → `allocb` → `putnext`.
- `close`: host `close`; OT's close waits for no host event.
- Lower side: accepts `I_LINK` from OT's configurator and ignores the linked `ip` *(verify the configurator's expectations in the Module Developer Note)*.
- About 1–1.5k lines of C per relay, most of it shared.

### 4.3 Replacement and installation

- ASLM resolves a class ID to one library; the relay libraries export the OT names with a newer version so ASLM prefers them over Apple's *(verify ASLM's rule for duplicate class IDs; fallbacks: an INIT that registers our file before OT's, or ASLM's own file-registration call)*.
- Install: two library files into `System Folder:Extensions` (type `libr`, own creator); no Apple file is modified. Removing them restores OT's own stacks.
- Apple's `ip`, `arp`, `dnr` still load; `ip` runs on the anchor port and `dnr` sends its queries through the `udp` relay.

### 4.4 Toolchain

| Need | Tool |
|---|---|
| `.ENET` driver, sResources | m68k `as` (our binutils) + a small declaration-ROM builder in the kernel build |
| ASLM relay libraries (C) | gcc for 68k (Retro68's, or ours) → ELF → **`mkaslm`**: emits the code resources (segment 0, `%A5Init`, `Main`), `libr`/`Libr`/`libi` and the file. Format learned clean-side from the shipped OT libraries and the ASLM Developer's Guide; proven by rebuilding a shipped library's resources byte for byte from its parsed form. K&R C, self-hosting, like `coff2elf` |
| OT/ASLM headers | user-supplied Universal Interfaces 3.x; only the constants and structures the relays use are restated in our own headers |
| Cross-check | MPW 3.5 + SC + ASLM SDK + OT Module SDK (user-supplied) in a Mac environment, building a sample module to compare with `mkaslm` output |

Retro68 builds classic 68k applications and code resources but not ASLM libraries (nor CFM-68K *(verify)*), and its free interfaces lack OT. CFM-68K is no alternative: OT does not load CFM-68K plug-ins.

### 4.5 Configuration

- **TCP/IP** control panel: "Connect via: Ethernet slot D", "Configure: Using DHCP Server" (anchor answers). One-time setting, done by the setup tool writing the preferences file or by the user.
- **AppleTalk** control panel: the same port. Under the relay (N4) the port is only OT's bookkeeping; the node comes from the host.
- DNS: servers and domain from DHCP (host `/etc/resolv.conf`); host names from a generated `Hosts` file in the System Folder (from `/etc/hosts`). NIS host lookups, which A/UX's MacTCP honours, need a `dnr` relay (N5, optional).

## 5. AppleTalk under OT

| | Relay to the host stack (recommended) | Virtual port |
|---|---|---|
| Node | host's (shared with 7.x `Patch.067C` sessions and Unix tools) | Mac's own |
| Replaced | `OTModl$ddp`, `OTModl$nbp` | nothing |
| Kept | OT `atp`, `adsp`, `pap`, `zip` over the relay | whole OT stack |
| Host | A/UX `ddp`/`elap` modules (DLM) + `otddp` | station |

- Per OT DDP endpoint the relay opens `/dev/appletalk/ddp/socket`, pushes `otddp`, binds the socket (`DDP_IOC_BIND_SOCK`). `otddp` maps `T_BIND_REQ` to the bind ioctl, `T_UNITDATA_REQ` (OT `DDPAddress` + type) to an extended DDP datagram `write`, received datagrams to `T_UNITDATA_IND`, and answers configuration queries (`DDP_IOC_GET_CFG`, `ZIP_IOC_GET_CFG`) for OT's `OTATalkGetInfo` path. OT's AARP/RTMP stay idle.
- `atp`, `adsp`, `pap` are DDP clients above a TPI boundary, so they run unchanged over the relay; the host's `at_atp`/`adsp` modules are not used by 8.1.
- **NBP**: names registered by the Mac must answer lookups arriving at the host node's socket 2, which the host kernel's NBP handler owns. `OTModl$nbp` relays registration and lookup to the same host facility `Patch.067C` uses; how A/UX delivers those lookups is still open ([aux-appletalk-interface.md](aux-appletalk-interface.md), open points).
- Until the host AppleTalk DLM exists, 8.1 uses the virtual port for AppleTalk.

## 6. Stages

Runtime tests run in QEMU `q800` on our kernel unless stated. Static checks follow the SONIC driver's pattern (host build, simulation, `verify.sh`).

| # | Stage | Builds | Static pass | Runtime pass |
|---|---|---|---|---|
| N0 | Format tools | `mkaslm` parser/emitter; resource dumper | the three OT 1.3 libraries parse; re-emitted resources byte-identical; `epcf` tables decoded into §1.1 | — |
| N1 | `otbridge` + native harness | `otxti`, `/dev/otbridge` session and ring; `otbtest`, a native AMIX program speaking OT-dialect records | host build of the translation code: primitive, address and option tables against golden records; DLM `mkmod` checks | `otbtest` over loopback and SONIC: TCP connect/accept (`T_CONN_RES` by cookie), data both ways, `T_EXDATA`, orderly release, abort; UDP unit data; `T_ADDR_REQ`; option negotiate/check incl. unknown; bind < 1024 fails as a user and works as root; ring events and SIGPOLL; EAGAIN/writable cycle under a slow reader; 1000 endpoints opened and closed without leaks |
| N2 | Virtual port | sn stations and local switch; `/dev/otbridge` minor 1; `.ENET` driver + sResource in the declaration ROM; classic-network switch-off in the graft | sn host simulation extended: station CAM, local copy, source-MAC enforcement; driver assembled; sResource list checked by the Slot Manager harness | 8.1 in our environment: OT lists "Ethernet slot D"; ping and Fetch to the LAN and to the host with the Mac's own IP; Chooser sees a LAN AFP server; host traffic unaffected (`ifconfig`, `netstat`) |
| N3 | TCP/UDP relay | relay library via `mkaslm`; session library; anchor mode + DHCP responder | library resources compared with OT's layout; relay unit tests in the host build against `otxti` | 8.1: TCP/IP panel shows the host's address; a browser and telnet reach LAN hosts from the host's IP; a Mac server on port ≥ 1024 is reachable at the host's address; port 80 denied for a user, allowed for root; host `netstat` shows the Mac's endpoints as the user's; OT sync and async samples from IM:OT pass |
| N4 | AppleTalk relay | AppleTalk DLM (A/UX modules) + `otddp`; `ddp`/`nbp` relay library | `otddp` host build against recorded DDP datagrams | Chooser via the host node; a 7.0.1 session and an 8.1 session use AppleTalk at the same time; NBP registration of a Mac service visible from another machine |
| N5 | DNS and configuration | `Hosts` generator; preferences writer; optional `dnr` relay | generated files parse | name lookup follows host `/etc/resolv.conf` changes after an OT restart; `/etc/hosts` names resolve |

**Buildable now (no Mac OS running):** N0, all of N1, and the static parts of N2–N4 (sn station simulation, `.ENET` driver and sResource build, relay libraries built and checked against the shipped libraries' format). ASLM replacement mechanics can be tried early in QEMU `q800` booting a stock 8.1 with a relay whose back end loops TPI locally.

## 7. Open questions and risks

**Open questions**

1. ASLM's choice between two libraries exporting one class ID (version rule, load order); whether OT caches module lookups before Extensions load.
2. OT's configurator behaviour for a `tcp` driver that ignores its lower `ip` link; whether `ip` must be up for `tcp` endpoints to open at all.
3. OT 1.3's exact TPI dialect: private primitives, `T_INFO_ACK` values, disconnect reason encoding, OOB handling, `T_OPTDATA`. Needs the Module Developer Note and headers (user-supplied).
4. `mkaslm`: the ASLM code-resource format (A5 world, jump table, relocation) and the `libr`/`Libr`/`libi` fields.
5. The NuBus scanner's matching of a synthetic network sResource; `.ENET` driver resolution by `DRVR` 127 from a declaration ROM.
6. AMIX 2.1 inet: where privileged ports are checked, IP-level options, multicast.
7. NBP lookups for Mac-registered names on the host node (shared with the 7.x open point).
8. MacTCP compatibility on 68k OT 1.3 (which component provides it) and conflicts with `Patch.067C`'s `.ipp`.
9. Whether 7.6.1 runs with classic networking (then it keeps `Patch.067C`'s pass-through) or needs OT 1.1.x, and whether the relays work on OT 1.1.x.
10. Descriptor budget per Mac process (AMIX `NOFILES`) with OT endpoints, `Patch.067C`'s vfs files and sockets together.

**Risks**

1. **ASLM replacement fails** (questions 1–2). Fallback (d): anchor port plus a host-side TCP/UDP terminator (slirp-style, BSD-licensed code) ("router" mode); loses transparent listening.
2. **`mkaslm` effort**: reproducing ASLM's format is reverse engineering of shipped binaries; the MPW cross-check needs user-supplied tools.
3. **68k OT under the Mac environment**: ASLM and OT rely on Deferred Task, Time Manager and interrupt-level behaviour that `Patch.067C` virtualises; failures here block every OT stage, the virtual port included.
4. **Blocking**: any blocking Unix call in a relay freezes the Mac session; review every call path for `O_NDELAY`.
5. **Station support per NIC**: SONIC has CAM room; LANCE-based hosts need promiscuous mode and CPU.
6. **Anchor DHCP** answers only OT's `ip`; if the user configures TCP/IP manually with another address, outgoing relayed connections still use the host's address while OT reports the manual one.
7. **Licensing**: OT, ASLM, SDKs and Universal Interfaces are Apple's and user-supplied; our relays, tools and headers restate only interface constants.
