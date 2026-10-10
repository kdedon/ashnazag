# Provider adapter contract

`provider.c` owns accepted downstream messages until their complete record is written or the endpoint closes. BAD/WAIT from `send` retain caller ownership. Read/write callbacks return transferred bytes, zero for retry, negative for failure. The host adapter maps EINTR/EAGAIN to retry and read EOF to failure.

`pump` runs a bounded nonblocking pass. Call it from deferred readiness work, including upstream flow-control recovery. It serializes reentrant pumps. Callback code may close the endpoint; storage must remain valid until every callback and pump returns. Initialize provider and bootstrap objects to zero before first use. Reopen only after the previous pump returns.

`deliver` returns DONE only after consuming or copying the control/data bytes, WAIT when blocked, and BAD on failure. It must deliver atomically: WAIT retains the complete message for retry. Queues remain opaque. The adapter must resolve the actual upstream queue through verified MPS methods and provide the release/delivery callbacks. Fatal reads/writes close the endpoint; client error/hangup notification belongs to the adapter.

Bootstrap opens a session and nonblocking endpoint, pushes `otxti`, selects RMSGN, and attaches the session/cookie/protocol. Every failure rolls descriptors back. `join` attaches another endpoint to the same session for acceptor-cookie lookup. Cookies must be unique within the session. Close children before their owner; owner close returns WAIT while children remain. Keep bootstrap storage alive through all joined endpoints. Descriptor ownership must remain singular: the provider's close callback must release its bootstrap handle rather than independently close its fd.

Buffers total 141320 bytes per provider, plus state. Allocate caller-owned storage when opening an endpoint. Automatic Mac allocator selection and deferred scheduling require framework integration.

`install_contract` supplies the six-word install metadata with verified shipping TCP flags 0x1001/synchronization 3 and UDP flags 0x1001/synchronization 1. It accepts explicit callback tables and refuses missing required callbacks. The diagnostic library exports separate `GetOTauxtcpInstallInfo` and `GetOTauxudpInstallInfo` callbacks. Both use global synchronization level 4 to serialize their shared session state; Apple declares this level explicitly. Metadata construction does not prove manager binding or provider loading.

Shipping echo code uses a 52-byte queue mate offset, while an MPS method uses 64 bytes. The provider uses imported RD/WR methods and defines only the prefix through q_ptr20, independently corroborated by shipping TCP/UDP open callbacks. The old echo queue prefix is incompatible and must remain separate. The qinit/streamtab/installinfo layouts match the local shipping tables and Apple headers:

- [OpenTransportProtocol.h](http://www.cs.vu.nl/~eliens/research/media/lib-of-vs-libs-QTDevWin-CIncludes-OpenTransportProtocol.h.html)
- [OpenTransportKernel.h](http://www.cs.vu.nl/~eliens/research/media/lib-of-vs-libs-QTDevWin-CIncludes-OpenTransportKernel.h.html)

Run `test/provider-host.sh PRIVATE_OUTPUT` for sanitizer fixtures and AMIX 68020 builds. `test/install-evidence.py` checks locally extracted cd70 resources; it writes only metadata facts. Neither check qualifies a live Mac provider.

The original import ordinals and shipping install attribution were incorrect: A5 jump pointers map to raw disk entries at v-18, and import ordinals are one-based. The new resource replay checks named export linkage and shipping echo import call sites.
