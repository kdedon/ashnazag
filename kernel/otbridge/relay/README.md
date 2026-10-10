# Mac relay building blocks

`aslm-import.py record class version minimum symbol=ordinal...` emits an import record and stubs. Assemble it, link with `aslmrt.o`, then pass `-I record` to mkaslm. The manager must supply the library context before a stub runs. Bindings contain two longs, a word and an ordinal long (14 bytes).

`record.c` encodes records for otxti and exposes received fragments without copying them. An upstream fragment with zero control bytes continues the previous message when MORE was set. The caller owns and retains buffers while views or writes reference them.

`otr_tx_step` performs one write per call. Its callback returns bytes accepted, zero for would-block, or a negative fatal error. An A/UX adapter must map EINTR/EAGAIN to zero and schedule another deferred invocation. Partial writes advance the cursor; a later invocation resumes the remaining bytes. Close or a fatal error discards the endpoint's pending record.


`mps-imports.sh` binds the shipped `OTKrnl$mps` class, version/minimum 0x0105. Its corrected jump-table mapping accounts for the 16-byte disk header. The earlier ordinal table is superseded: allocb is7, freemsg24, getq28, putnext40, qprocson44, RD60 and WR61. The numerical evidence verifier checks targets against shipping function names. Native pointer imports explicitly consume D0 before returning a pointer through the compiler ABI; nine compiled wrappers check that conversion.

`otr_message` flattens DATA or PROTO/PCPROTO plus DATA continuations into one bridge record. It validates block bounds, refuses unsupported bands/flags and limits traversal to 64 blocks. It preserves message ownership and read pointers, so callers can retain a message during backpressure.

Message layouts come from the public [Apple Protocol interface](http://www.cs.vu.nl/~eliens/research/media/lib-of-vs-libs-QTDevWin-CIncludes-OpenTransportProtocol.h.html). The shipped echo module independently confirms continuation/read/datab/band offsets 8/12/20/24 and datab type13. Compile-time checks enforce the 68020 representation. These are message-prefix checks; queue ABI differs: the shipped echo callback uses a 52-byte mate offset, while MPS uses64 bytes and the later public header declares64. The adapter uses verified shared-prefix fields and native RD/WR helpers, avoiding mate-size arithmetic. Full queue/private fields require separate version checks.
