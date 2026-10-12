#ifndef __FX_INLINE_H__
#define __FX_INLINE_H__
#include <stddef.h>
#define kTriProcOffset offsetof(struct GrGC_s, triSetupProc)
#define kInternalFogTableEntryCount (sizeof(((SstRegs *)0)->fogTable) >> 1)
#define kTLSOffset offsetof(struct _GlideRoot_s, tlsOffset)
#define kLostContextOffset offsetof(GrGC, lostContext)
#endif
