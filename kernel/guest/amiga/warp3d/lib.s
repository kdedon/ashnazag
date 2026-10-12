| Warp3D.library: resident tag and one stub per library vector.  Each
| stub hands its vector number to glue, which calls the host function
| startmig placed in its table (w3dshare.h) with the vector's registers
| as C arguments, on the host stack, under the library's semaphore.

 .set TAB, 0x25000000
 .set T_STACK, 12
 .set T_RING, 16
 .set T_SPOILT, 44
 .set T_FN, 64
 .set T_CALLS, 400
 .set B_SEM, 36

 .text
 .globl _start
_start:
 moveq #-1,%d0
 rts

romtag:
 .word 0x4afc
 .long romtag, endskip
 .byte 0x80, 4, 9, 0		| RTF_AUTOINIT, version, NT_LIBRARY, priority
 .long w3d_name, w3d_idstring, w3d_autoinit

 .globl linit
linit:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l %a0,-(%sp)
 move.l %d0,-(%sp)
 jsr w3d_init
 addq.l #8,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts
 .macro lib fn
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l %a6,-(%sp)
 jsr \fn
 addq.l #4,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts
 .endm
lopen: lib w3d_open
lclose: lib w3d_close
lexpunge: lib w3d_expunge
lnull:
 moveq #0,%d0
 rts

| vector n, argument count, registers in C order (d0-d7 0-7, a0-a6 8-14)
 .macro lvo n, cnt, regs:vararg
 .globl l\n
l\n:
 movem.l %d0-%d7/%a0-%a6,-(%sp)
 moveq #\n,%d0
 bra glue
 .pushsection .rodata.desc
 .byte \cnt
 .irp r,\regs
 .byte \r
 .endr
 .fill 7-\cnt,1,0
 .popsection
 .endm

 .section .rodata.desc,"a"
 .balign 8
desc:
 .text

| the caller's registers at (sp), d0 the vector
glue:
 move.l %sp,%a5
 move.l %a6,%a4
 move.l %d0,%d7
 lea TAB,%a3
 lsl.l #2,%d0
 lea T_CALLS(%a3),%a0
 addq.l #1,(%a0,%d0.l)
 move.l 4.w,%a6
 lea B_SEM(%a4),%a0
 jsr -564(%a6)			| ObtainSemaphore
 move.l %a5,-(%sp)
 move.l %a4,-(%sp)
 move.l %d7,-(%sp)
 jsr w3d_pre
 lea 12(%sp),%sp
 fmove.l %fpcr,%d6
 moveq #0,%d0
 fmove.l %d0,%fpcr
 move.l T_STACK(%a3),%sp
 lea desc,%a0
 move.l %d7,%d0
 lsl.l #3,%d0
 add.l %d0,%a0
 moveq #0,%d2
 move.b (%a0)+,%d2
 add.l %d2,%a0
 bra.s 2f
1: moveq #0,%d1
 move.b -(%a0),%d1
 move.l (%a5,%d1.l*4),-(%sp)
2: dbra %d2,1b
 move.l %d7,%d0
 lsl.l #2,%d0
 move.l T_FN(%a3,%d0.l),%a0
 move.l 4.w,%d5
 jsr (%a0)
 cmp.l 4.w,%d5			| host code wrote through a null pointer?
 beq.s 4f
 move.l %d5,-(%sp)
 move.l %d7,-(%sp)
 move.l T_SPOILT(%a3),%a0
 jsr (%a0)
4: move.l %a5,%sp
 move.l %d0,(%sp)
 move.l %d0,32(%sp)
 fmove.l %d6,%fpcr
 move.l 4.w,%a6
 lea B_SEM(%a4),%a0
 jsr -570(%a6)			| ReleaseSemaphore
 tst.l T_RING(%a3)
 beq.s 3f
 clr.l T_RING(%a3)
 move.l #2,0xf7fffc		| the display's doorbell: it copies the screen
3: movem.l (%sp)+,%d0-%d7/%a0-%a6
 rts

 lvo 0, 2, 8, 9	| CreateContext
 lvo 1, 1, 8	| DestroyContext
 lvo 2, 2, 8, 1	| GetState
 lvo 3, 3, 8, 0, 1	| SetState
 lvo 4, 0	| CheckDriver
 lvo 5, 1, 8	| LockHardware
 lvo 6, 1, 8	| UnLockHardware
 lvo 7, 1, 8	| WaitIdle
 lvo 8, 1, 8	| CheckIdle
 lvo 9, 3, 8, 0, 1	| Query
 lvo 10, 3, 8, 0, 1	| GetTexFmtInfo
 lvo 11, 3, 8, 9, 10	| AllocTexObj
 lvo 12, 2, 8, 9	| FreeTexObj
 lvo 13, 2, 8, 9	| ReleaseTexture
 lvo 14, 1, 8	| FlushTextures
 lvo 15, 4, 8, 9, 0, 1	| SetFilter
 lvo 16, 4, 8, 9, 1, 10	| SetTexEnv
 lvo 17, 5, 8, 9, 0, 1, 10	| SetWrapMode
 lvo 18, 5, 8, 9, 10, 1, 11	| UpdateTexImage
 lvo 19, 2, 8, 9	| UploadTexture
 lvo 20, 2, 8, 9	| DrawLine
 lvo 21, 2, 8, 9	| DrawPoint
 lvo 22, 2, 8, 9	| DrawTriangle
 lvo 23, 2, 8, 9	| DrawTriFan
 lvo 24, 2, 8, 9	| DrawTriStrip
 lvo 25, 3, 8, 1, 9	| SetAlphaMode
 lvo 26, 3, 8, 0, 1	| SetBlendMode
 lvo 27, 4, 8, 9, 1, 10	| SetDrawRegion
 lvo 28, 3, 8, 9, 1	| SetFogParams
 lvo 29, 5, 8, 0, 1, 2, 3	| SetColorMask
 lvo 30, 4, 8, 0, 1, 2	| SetStencilFunc
 lvo 31, 1, 8	| AllocZBuffer
 lvo 32, 1, 8	| FreeZBuffer
 lvo 33, 2, 8, 9	| ClearZBuffer
 lvo 34, 4, 8, 0, 1, 9	| ReadZPixel
 lvo 35, 5, 8, 0, 1, 2, 9	| ReadZSpan
 lvo 36, 2, 8, 1	| SetZCompareMode
 lvo 37, 1, 8	| AllocStencilBuffer
 lvo 38, 2, 8, 9	| ClearStencilBuffer
 lvo 39, 7, 8, 0, 1, 2, 3, 4, 9	| FillStencilBuffer
 lvo 40, 1, 8	| FreeStencilBuffer
 lvo 41, 4, 8, 0, 1, 9	| ReadStencilPixel
 lvo 42, 5, 8, 0, 1, 2, 9	| ReadStencilSpan
 lvo 43, 2, 8, 1	| SetLogicOp
 lvo 44, 3, 8, 0, 1	| Hint
 lvo 45, 3, 8, 9, 10	| SetDrawRegionWBM
 lvo 46, 1, 8	| GetDriverState
 lvo 47, 1, 8	| Flush
 lvo 48, 2, 8, 1	| SetPenMask
 lvo 49, 4, 8, 0, 1, 2	| SetStencilOp
 lvo 50, 2, 8, 1	| SetWriteMask
 lvo 51, 4, 8, 0, 1, 2	| WriteStencilPixel
 lvo 52, 6, 8, 0, 1, 2, 9, 10	| WriteStencilSpan
 lvo 53, 4, 8, 0, 1, 9	| WriteZPixel
 lvo 54, 6, 8, 0, 1, 2, 9, 10	| WriteZSpan
 lvo 55, 2, 8, 9	| SetCurrentColor
 lvo 56, 2, 8, 1	| SetCurrentPen
 lvo 57, 7, 8, 9, 10, 1, 11, 12, 0	| UpdateTexSubImage
 lvo 58, 1, 8	| FreeAllTexObj
 lvo 59, 0	| GetDestFmt
 lvo 60, 2, 8, 9	| DrawLineStrip
 lvo 61, 2, 8, 9	| DrawLineLoop
 lvo 62, 0	| GetDrivers
 lvo 63, 3, 8, 0, 1	| QueryDriver
 lvo 64, 3, 8, 0, 1	| GetDriverTexFmtInfo
 lvo 65, 1, 8	| RequestMode
 lvo 66, 2, 8, 9	| SetScissor
 lvo 67, 1, 8	| FlushFrame
 lvo 68, 1, 0	| TestMode
 lvo 69, 5, 8, 9, 0, 1, 2	| SetChromaTestBounds
 lvo 70, 2, 8, 0	| ClearDrawRegion
 lvo 71, 2, 8, 9	| DrawTriangleV
 lvo 72, 2, 8, 9	| DrawTriFanV
 lvo 73, 2, 8, 9	| DrawTriStripV
 lvo 74, 0	| GetScreenmodeList
 lvo 75, 1, 8	| FreeScreenmodeList
 lvo 76, 1, 8	| BestModeID
 lvo 77, 5, 8, 9, 0, 1, 2	| VertexPointer
 lvo 78, 7, 8, 9, 0, 1, 2, 3, 4	| TexCoordPointer
 lvo 79, 6, 8, 9, 0, 1, 2, 3	| ColorPointer
 lvo 80, 3, 8, 0, 9	| BindTexture
 lvo 81, 4, 8, 0, 1, 2	| DrawArray
 lvo 82, 5, 8, 0, 1, 2, 9	| DrawElements
 lvo 83, 2, 8, 0	| SetFrontFace

 .balign 4
 .globl vectors
vectors:
 .long lopen, lclose, lexpunge, lnull
 .long l0
 .long l1
 .long l2
 .long l3
 .long l4
 .long l5
 .long l6
 .long l7
 .long l8
 .long l9
 .long l10
 .long l11
 .long l12
 .long l13
 .long l14
 .long l15
 .long l16
 .long l17
 .long l18
 .long l19
 .long l20
 .long l21
 .long l22
 .long l23
 .long l24
 .long l25
 .long l26
 .long l27
 .long l28
 .long l29
 .long l30
 .long l31
 .long l32
 .long l33
 .long l34
 .long l35
 .long l36
 .long l37
 .long l38
 .long l39
 .long l40
 .long l41
 .long l42
 .long l43
 .long l44
 .long l45
 .long l46
 .long l47
 .long l48
 .long l49
 .long l50
 .long l51
 .long l52
 .long l53
 .long l54
 .long l55
 .long l56
 .long l57
 .long l58
 .long l59
 .long l60
 .long l61
 .long l62
 .long l63
 .long l64
 .long l65
 .long l66
 .long l67
 .long l68
 .long l69
 .long l70
 .long l71
 .long l72
 .long l73
 .long l74
 .long l75
 .long l76
 .long l77
 .long l78
 .long l79
 .long l80
 .long l81
 .long l82
 .long l83
 .long -1
endskip:

| long libcall(base, lvo, d0, d1, a0, a1)
 .globl libcall
libcall:
 movem.l %d2/%a2/%a6,-(%sp)
 move.l 16(%sp),%a6
 move.l 20(%sp),%d2
 move.l 24(%sp),%d0
 move.l 28(%sp),%d1
 move.l 32(%sp),%a0
 move.l 36(%sp),%a1
 jsr 0(%a6,%d2.l)
 movem.l (%sp)+,%d2/%a2/%a6
 rts

 .section .note.GNU-stack,"",@progbits
