#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
exec python3 "$D/aslm-import.py" mps_import 'OTKrnl$mps' 0x105 0x105 \
    OTAllocMem_D0=4 OTFreeMem=6 allocb_D0=7 bcanput=10 bcanputnext=11 \
    bufcall=12 canput=13 canputnext=14 flushq=22 freeb=23 freemsg=24 \
    getq_D0=28 putbq=34 putnextctl=39 putnext=40 putnextctl1=41 putq=42 \
    qenable=43 qprocson=44 qprocsoff=45 qreply=46 noenable=58 \
    OTHERQ=59 RD_D0=60 WR_D0=61 mi_timer_cancel=62 mi_timer=63 \
    mi_timer_alloc_D0=64 mi_timer_free=65 mi_timer_valid=66 unbufcall=53
