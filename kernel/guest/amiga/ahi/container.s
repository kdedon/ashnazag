| container.audio -- an AHI driver for the Amiga environment.  AHI mixes;
| a task of this driver runs the player and mixer hooks whenever the
| host's sound helper rings (PORTS, its doorbell set) and puts the mixed
| 16-bit frames in the shared ring the helper sends to the host's sound
| service.  Start and Stop ring the helper's doorbell.

.equ BASE, 0x23000000
.equ MAGIC, 0x4d534e44
.equ RING, 0x40000
.equ RINGDATA, BASE+4096
.equ BELL, 0xf7fff8
.equ s_ready, 8
.equ s_play, 12
.equ s_gen, 16
.equ s_rate, 20
.equ s_chans, 24
.equ s_head, 28
.equ s_tail, 32
.equ s_want, 36
.equ s_doorbell, 40
.equ s_passes, 52

| struct AHIAudioCtrlDrv
.equ ac_Flags, 4
.equ ac_PlayerFunc, 12
.equ ac_PlayerFreq, 16
.equ ac_MixFreq, 28
.equ ac_MixerFunc, 40
.equ ac_BuffSamples, 52
.equ ac_MaxBuffSamples, 60
.equ ac_BuffSize, 64
.equ ac_BuffType, 68
.equ ac_PreTimer, 72
.equ ac_PostTimer, 76

.equ LIB_FLAGS, 14
.equ LIB_NEGSIZE, 16
.equ LIB_POSSIZE, 18
.equ LIB_OPENCNT, 32
.equ lib_exec, 36
.equ lib_segment, 40
.equ lib_size, 44

.equ TASKSIZE, 92
.equ STACK, 8192
.equ SIGKICK, 16
.equ TAG, 0x80000000

.text
.globl _start
_start:
 moveq #-1,%d0
 rts
resident:
 .word 0x4afc
 .long resident, code_end
 .byte 0x80, 4, 9, 0
 .long library_name, id_string, init_table
library_name: .asciz "container.audio"
id_string: .asciz "container.audio 4.1 (06.10.2026)"
 .asciz "$VER: container.audio 4.1 (06.10.2026)"
author: .asciz "Ash Nazag"
copyright: .asciz "GPL-2.0"
annotation: .asciz "The host's sound service, through the Amiga environment."
outname: .asciz "Host"
taskname: .asciz "container.audio"
 .balign 4
init_table:
 .long lib_size, vectors, 0, library_init
vectors:
 .long library_open, library_close, library_expunge, return_zero
 .long alloc_audio, free_audio, disable, enable, start, update, stop
 .long unknown, unknown, unknown, unknown, unknown, unknown
 .long get_attr, return_zero, -1
library_init:
 move.l %d0,%a1
 move.l %a6,lib_exec(%a1)
 move.l %a0,lib_segment(%a1)
 move.b #9,8(%a1)
 move.l #library_name,10(%a1)
 move.b #6,LIB_FLAGS(%a1)
 move.w #4,20(%a1)
 move.w #1,22(%a1)
 move.l #id_string,24(%a1)
 rts
library_open:
 addq.w #1,LIB_OPENCNT(%a6)
 bclr #3,LIB_FLAGS(%a6)
 move.l %a6,%d0
 rts
library_close:
 subq.w #1,LIB_OPENCNT(%a6)
 bne return_zero
 btst #3,LIB_FLAGS(%a6)
 bne library_expunge
return_zero:
 moveq #0,%d0
 rts
library_expunge:
 tst.w LIB_OPENCNT(%a6)
 bne defer_expunge
 tst.l ctrl
 bne defer_expunge
 movem.l %d2/%a5-%a6,-(%sp)
 move.l %a6,%a5
 move.l lib_exec(%a5),%a6
 move.l lib_segment(%a5),%d2
 move.l %a5,%a1
 jsr -252(%a6)
 moveq #0,%d0
 move.w LIB_NEGSIZE(%a5),%d0
 move.l %a5,%a1
 sub.l %d0,%a1
 add.w LIB_POSSIZE(%a5),%d0
 jsr -210(%a6)
 move.l %d2,%d0
 movem.l (%sp)+,%d2/%a5-%a6
 rts
defer_expunge:
 bset #3,LIB_FLAGS(%a6)
 bra return_zero
| SetVol, SetFreq, SetSound, SetEffect, LoadSound, UnloadSound: AHI's own
unknown:
 moveq #-1,%d0
 rts

| AllocAudio(a1 tags, a2 audioctrl): one user, with the helper running
alloc_audio:
 move.l #BASE,%a0
 cmp.l #MAGIC,(%a0)
 bne 9f
 cmp.l #1,4(%a0)
 bne 9f
 tst.l s_ready(%a0)
 beq 9f
 tst.l ctrl
 bne 9f
 move.l %a2,ctrl
 move.l ac_MixFreq(%a2),%d0
 cmp.l #8000,%d0
 bcc 1f
 move.l #8000,%d0
1: cmp.l #48000,%d0
 bls 2f
 move.l #48000,%d0
2: move.l %d0,ac_MixFreq(%a2)
 | mixing, timing, stereo
 moveq #14,%d0
 rts
9: moveq #1,%d0
 rts

free_audio:
 cmp.l ctrl,%a2
 bne 1f
 moveq #1,%d0
 bsr stop
 clr.l ctrl
1: rts

disable:
 move.l %a6,-(%sp)
 move.l 4.w,%a6
 jsr -132(%a6)
 move.l (%sp)+,%a6
 rts
enable:
 move.l %a6,-(%sp)
 move.l 4.w,%a6
 jsr -138(%a6)
 move.l (%sp)+,%a6
 rts

| ahiac_BuffSamples: the mixing frequency over the player's, within the buffer
update:
 movem.l %d1-%d2,-(%sp)
 move.l ac_PlayerFreq(%a2),%d1
 bne 4f
 move.l #50,%d1
4: cmp.l #65536,%d1
 bcc 1f
 swap %d1
 clr.w %d1
1: move.l ac_MixFreq(%a2),%d0
 swap %d0
 clr.w %d0
 divu.l %d1,%d0
 bne 2f
 moveq #1,%d0
2: move.l cap,%d2
 beq 3f
 cmp.l %d2,%d0
 bls 3f
 move.l %d2,%d0
3: move.l %d0,ac_BuffSamples(%a2)
 movem.l (%sp)+,%d1-%d2
 moveq #0,%d0
 rts

| Start(d0 flags, a2 audioctrl): play only; a new task and buffer each time
start:
 btst #0,%d0
 beq return_zero
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 bsr stop_play
 move.l 4.w,%a6
 | the mixer's samples: 32-bit for types 8 and 10, stereo for 3 and 10
 move.l ac_BuffType(%a2),%d0
 moveq #1,%d2
 cmp.l #3,%d0
 beq 1f
 cmp.l #10,%d0
 beq 1f
 cmp.l #1,%d0
 beq 2f
 cmp.l #8,%d0
 beq 2f
 btst #2,ac_Flags+3(%a2)
 beq 2f
1: moveq #2,%d2
2: move.l %d2,chans
 sf wide
 cmp.l #8,%d0
 bcs 3f
 st wide
3: | the largest buffer AHI may ask for: its size, or 8 bytes a frame
 move.l ac_MaxBuffSamples(%a2),%d0
 lsl.l #3,%d0
 cmp.l ac_BuffSize(%a2),%d0
 bcc 4f
 move.l ac_BuffSize(%a2),%d0
4: cmp.l #4096,%d0
 bcc 5f
 move.l #4096,%d0
5: move.l %d0,mixsize
 lsr.l #3,%d0
 move.l %d0,cap
 move.l mixsize,%d0
 move.l #0x10001,%d1
 jsr -684(%a6)
 move.l %d0,mixbuf
 beq 8f
 bsr update
 | the task, its stack after it
 move.l #TASKSIZE+STACK,%d0
 move.l #0x10001,%d1
 jsr -198(%a6)
 move.l %d0,taskmem
 beq 7f
 move.l %d0,%a1
 move.b #1,8(%a1)
 move.b #25,9(%a1)
 move.l #taskname,10(%a1)
 move.l #0x1ffff,18(%a1)
 lea TASKSIZE(%a1),%a0
 move.l %a0,58(%a1)
 lea STACK(%a0),%a0
 move.l %a0,62(%a1)
 move.l %a0,54(%a1)
 | the ring from here on, then the helper's doorbell
 move.l #BASE,%a5
 move.l ac_MixFreq(%a2),s_rate(%a5)
 move.l chans,s_chans(%a5)
 move.l s_tail(%a5),s_head(%a5)
 addq.l #1,s_gen(%a5)
 move.l #1,s_play(%a5)
 move.l #1,BELL
 move.l %a1,task
 move.l %a1,sigtask
 move.l #1<<SIGKICK,sigmask
 lea player(%pc),%a2
 sub.l %a3,%a3
 jsr -282(%a6)
 lea ports(%pc),%a1
 moveq #3,%d0
 jsr -168(%a6)
 move.l task,%a1
 move.l #1<<SIGKICK,%d0
 jsr -324(%a6)
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
7: move.l mixbuf,%a1
 jsr -690(%a6)
 clr.l mixbuf
8: movem.l (%sp)+,%d2-%d7/%a2-%a6
 | AHIE_NOMEM
 moveq #1,%d0
 rts

| Stop(d0 flags, a2 audioctrl)
stop:
 btst #0,%d0
 beq return_zero
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 bsr stop_play
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
| the task told to quit and gone, its memory freed, the helper rung
stop_play:
 move.l %a6,-(%sp)
 move.l 4.w,%a6
 tst.l task
 beq 1f
 lea ports(%pc),%a1
 moveq #3,%d0
 jsr -174(%a6)
 move.l #BASE,%a0
 clr.l s_play(%a0)
 move.l #1,BELL
 sub.l %a1,%a1
 jsr -294(%a6)
 move.l %d0,stopper
 moveq #0,%d0
 moveq #16,%d1
 jsr -306(%a6)
 move.l task,%a1
 move.l #0x1000,%d0
 jsr -324(%a6)
 moveq #16,%d0
 jsr -318(%a6)
 clr.l task
 move.l taskmem,%a1
 move.l #TASKSIZE+STACK,%d0
 jsr -210(%a6)
 move.l mixbuf,%a1
 jsr -690(%a6)
 clr.l mixbuf
 clr.l cap
1: move.l (%sp)+,%a6
 rts

| The task: a pass per signal from the PORTS server while the ring is low
player:
 move.l 4.w,%a6
 move.l ctrl,%a2
 move.l #BASE,%a5
1: move.l #(1<<SIGKICK)+0x1000,%d0
 jsr -318(%a6)
2: btst #12,%d0
 bne 9f
 tst.l s_play(%a5)
 beq 1b
 move.l s_head(%a5),%d0
 sub.l s_tail(%a5),%d0
 cmp.l s_want(%a5),%d0
 bcc 1b
 move.l ac_BuffSamples(%a2),%d1
 cmp.l cap,%d1
 bls 3f
 move.l cap,%d1
3: move.l %d1,%d7
 lsl.l #1,%d1
 cmp.l #2,chans
 bne 4f
 lsl.l #1,%d1
4: add.l %d1,%d0
 cmp.l #RING,%d0
 bhi 1b
 bsr mix
 | quit between passes if asked
 moveq #0,%d0
 moveq #0,%d1
 jsr -306(%a6)
 bra 2b
9: jsr -132(%a6)
 move.l stopper,%a1
 moveq #16,%d0
 jsr -324(%a6)
 rts

| one pass of d7 frames: player, mixer, into the ring
mix:
 addq.l #1,s_passes(%a5)
 move.l ac_PlayerFunc(%a2),%d0
 beq 1f
 move.l %d0,%a0
 sub.l %a1,%a1
 move.l 8(%a0),%a3
 jsr (%a3)
1: move.l ac_PreTimer(%a2),%d0
 beq 2f
 move.l %d0,%a0
 jsr (%a0)
 tst.w %d0
 bne 3f
2: move.l ac_MixerFunc(%a2),%d0
 beq 3f
 move.l %d0,%a0
 move.l mixbuf,%a1
 move.l 8(%a0),%a3
 jsr (%a3)
 bra 4f
| too busy: silence keeps the time
3: move.l mixbuf,%a0
 move.l mixsize,%d0
 lsr.l #2,%d0
 bra 31f
30: clr.l (%a0)+
31: subq.l #1,%d0
 bpl 30b
4: move.l ac_PostTimer(%a2),%d0
 beq 5f
 move.l %d0,%a0
 jsr (%a0)
5: move.l %d7,%d0
 move.l chans,%d1
 subq.l #1,%d1
 lsl.l %d1,%d0
 move.l mixbuf,%a0
 lea RINGDATA,%a1
 move.l s_head(%a5),%d1
 and.l #RING-1,%d1
 tst.b wide
 bne 7f
 bra 61f
60: move.w (%a0)+,(%a1,%d1.l)
 addq.l #2,%d1
 and.l #RING-1,%d1
61: subq.l #1,%d0
 bpl 60b
 bra 8f
70: move.w (%a0),(%a1,%d1.l)
 addq.l #4,%a0
 addq.l #2,%d1
 and.l #RING-1,%d1
7: subq.l #1,%d0
 bpl 70b
8: move.l %d7,%d0
 lsl.l #1,%d0
 cmp.l #2,chans
 bne 9f
 lsl.l #1,%d0
9: add.l %d0,s_head(%a5)
 rts

| GetAttr(d0 attribute, d1 argument, d2 default, a1 tags, a2 audioctrl)
get_attr:
 lea attrs(%pc),%a0
1: tst.l (%a0)
 beq 2f
 cmp.l (%a0)+,%d0
 beq 3f
 addq.l #4,%a0
 bra 1b
3: move.l (%a0),%d0
 rts
2: cmp.l #TAG+117,%d0
 bne 4f
 | AHIDB_Frequency: the argument's
 cmp.l #NFREQ,%d1
 bcs 5f
 moveq #NFREQ-1,%d1
5: lea freqs(%pc),%a0
 move.l (%a0,%d1.l*4),%d0
 rts
4: cmp.l #TAG+124,%d0
 bne 6f
 | AHIDB_Index: the highest not above the argument
 lea freqs(%pc),%a0
 moveq #0,%d0
7: cmp.l #NFREQ-1,%d0
 beq 8f
 cmp.l 4(%a0,%d0.l*4),%d1
 bcs 8f
 addq.l #1,%d0
 bra 7b
8: rts
6: move.l %d2,%d0
 rts

 .balign 4
freqs: .long 8000, 11025, 16000, 22050, 32000, 44100, 48000
.equ NFREQ, 7
attrs:
 .long TAG+110, 16
 .long TAG+115, NFREQ
 .long TAG+118, author
 .long TAG+119, copyright
 .long TAG+120, id_string
 .long TAG+121, annotation
 .long TAG+114, 0
 .long TAG+129, 0
 .long TAG+125, 1
 .long TAG+127, 0
 .long TAG+130, 0
 .long TAG+131, 0
 .long TAG+132, 0x10000
 .long TAG+133, 0x10000
 .long TAG+134, 0x10000
 .long TAG+135, 0x10000
 .long TAG+136, 0
 .long TAG+139, 1
 .long TAG+141, outname
 .long 0

| PORTS: the helper rang; the task mixes.  The chain goes on.
pserver:
 move.l #BASE,%a0
 tst.l s_doorbell(%a0)
 beq 1f
 clr.l s_doorbell(%a0)
 move.l 4(%a1),%d0
 move.l (%a1),%a1
 jsr -324(%a6)
1: lea 0xdff000,%a0
 moveq #0,%d0
 rts

 .balign 4
ports: .long 0, 0
 .byte 2, 0
 .long taskname, sigtask, pserver
sigtask: .long 0
sigmask: .long 0
ctrl: .long 0
task: .long 0
taskmem: .long 0
stopper: .long 0
mixbuf: .long 0
mixsize: .long 0
cap: .long 0
chans: .long 0
wide: .byte 0
 .balign 4
code_end:
