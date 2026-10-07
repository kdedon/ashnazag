.include "handler-image.inc"
.text
.globl extension_resident
extension_resident:
 .word 0x4afc
 .long extension_resident,rtg_resident
 .byte 1,1,0,-39
 .long extension_name,extension_id,extension_init
extension_name: .asciz "container.boot"
extension_id: .asciz "container.boot 1.0"
expansion_name: .asciz "expansion.library"
dos_name: .asciz "dos.library"
.balign 4
extension_init:
 movem.l %d2-%d5/%a2-%a6,-(%sp)
 move.l 4,%a6
 lea expansion_name,%a1
 moveq #36,%d0
 jsr -552(%a6)
 tst.l %d0
 beq init_done
 move.l %d0,%a5
 move.l #HANDLER_SIZE+8,%d0
 move.l #65537,%d1
 jsr -198(%a6)
 tst.l %d0
 beq close_expansion
 move.l %d0,%a3
 move.l #HANDLER_SIZE+8,(%a3)
 lea 8(%a3),%a2
 move.l %a2,%a1
 lea handler_image,%a0
 move.l #HANDLER_SIZE/4,%d0
copy_handler:
 move.l (%a0)+,(%a1)+
 subq.l #1,%d0
 bne copy_handler
 lea handler_relocations,%a0
 move.l #HANDLER_RELOCS,%d0
 tst.l %d0
 beq relocated
 move.l %a2,%d2
relocate_handler:
 move.l (%a0)+,%d1
 add.l %d2,(%a2,%d1.l)
 subq.l #1,%d0
 bne relocate_handler
relocated:
 jsr -636(%a6)
 move.l %a5,%a6
 jsr -48(%a6)
 tst.l %d0
 beq free_handler
 move.l %d0,%a4
 move.b #0x10,16(%a4)
 move.l #diagnostic,28(%a4)
 move.l #0xf00000,32(%a4)
 move.l #0x80000,36(%a4)
 lea 4(%a3),%a0
 move.l %a0,%d0
 lsr.l #2,%d0
 move.l %d0,-(%sp)
 move.l %a4,-(%sp)
 jsr register_boot
 addq.l #8,%sp
 tst.l %d0
 beq free_config
 move.l %a4,%a0
 move.l %a5,%a6
 jsr -30(%a6)
 bra close_expansion
free_config:
 move.l %a4,%a0
 move.l %a5,%a6
 jsr -84(%a6)
free_handler:
 move.l 4,%a6
 move.l %a3,%a1
 move.l #HANDLER_SIZE+8,%d0
 jsr -210(%a6)
close_expansion:
 move.l 4,%a6
 move.l %a5,%a1
 jsr -414(%a6)
init_done:
 movem.l (%sp)+,%d2-%d5/%a2-%a6
 moveq #0,%d0
 rts
.balign 4
diagnostic:
 .byte 0x10,0
 .word diagnostic_end-diagnostic,0,boot_entry-diagnostic,0,0,0
boot_entry:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4,%a6
 lea dos_name,%a1
 jsr -96(%a6)
 tst.l %d0
 beq boot_failed
 move.l %d0,%a1
 moveq #0,%d1
 jsr -102(%a6)
boot_failed:
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #-1,%d0
 rts
diagnostic_end:
.balign 4
/* After DOS has assigned SYS: and DEVS:, bind the RTG board before
 * anything opens a screen, so Workbench opens on it. */
rtg_resident:
 .word 0x4afc
 .long rtg_resident,rtg_res_name
 .byte 4,1,0,-110
 .long rtg_res_name,rtg_res_name,rtg_init
rtg_res_name: .asciz "container.rtg"
icon_name: .asciz "icon.library"
rtg_name: .asciz "Picasso96/rtg.library"
monitor_path: .asciz "DEVS:Monitors/Container"
board: .asciz "Container"
env_path: .asciz "ENV:"
env_name: .asciz "ENV"
envarc_path: .asciz "SYS:Prefs/Env-Archive"
watch_name: .asciz "container.wb"
intuition_name: .asciz "intuition.library"
graphics_name: .asciz "graphics.library"
.balign 4
rtg_init:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l 4,%a6
 sub.l %a1,%a1
 jsr -294(%a6)
 move.l %d0,%a4
 cmp.b #13,8(%a4)
 bne rtg_done
 /* no requesters: a missing file just leaves the native display */
 move.l 184(%a4),-(%sp)
 moveq #-1,%d0
 move.l %d0,184(%a4)
 lea dos_name,%a1
 moveq #36,%d0
 jsr -552(%a6)
 move.l %d0,%d7
 beq rtg_restore
 move.l %d7,%a6
 /* P96 reads its variables from ENV:; until startup copies the
  * saved environment, ENV: is the saved one */
 move.l #env_path,%d1
 moveq #-2,%d2
 jsr -84(%a6)
 move.l %d0,%d1
 beq env_missing
 jsr -90(%a6)
 bra env_ready
env_missing:
 move.l #envarc_path,%d1
 moveq #-2,%d2
 jsr -84(%a6)
 move.l %d0,%d2
 beq env_ready
 move.l #env_name,%d1
 jsr -612(%a6)
 tst.l %d0
 bne env_ready
 move.l %d2,%d1
 jsr -90(%a6)
env_ready:
 /* bound before DOS: start what it deferred, then only watch Workbench */
 move.l %d7,-(%sp)
 jsr early_late
 addq.l #4,%sp
 tst.l %d0
 beq 1f
 bsr start_watch
 bra rtg_close_dos
1: move.l 4,%a6
 lea icon_name,%a1
 moveq #36,%d0
 jsr -552(%a6)
 move.l %d0,%d6
 beq rtg_close_dos
 move.l %d6,%a6
 lea monitor_path,%a0
 jsr -78(%a6)
 move.l %d0,%d5
 beq rtg_close_icon
 move.l 4,%a6
 lea rtg_name,%a1
 moveq #40,%d0
 jsr -552(%a6)
 move.l %d0,%d4
 beq rtg_free_icon
 /* what the monitor driver does: board context from its tooltypes, then modes */
 move.l %d5,%a0
 clr.l -(%sp)
 move.l 54(%a0),-(%sp)
 move.l #0x8000415c,-(%sp)
 lea board,%a0
 move.l %sp,%a1
 move.l %d4,%a6
 jsr -30(%a6)
 lea 12(%sp),%sp
 tst.l %d0
 beq rtg_close
 move.l %d0,%a0
 jsr -36(%a6)
 tst.l %d0
 beq rtg_close
 jsr early_alerts
 bsr start_watch
 bra rtg_close
/* CreateNewProc: NP_Entry, NP_Name, NP_StackSize */
start_watch:
 move.l %a6,-(%sp)
 move.l %d7,%a6
 clr.l -(%sp)
 move.l #4096,-(%sp)
 move.l #0x800003f3,-(%sp)
 move.l #watch_name,-(%sp)
 move.l #0x800003f4,-(%sp)
 move.l #wb_watch,-(%sp)
 move.l #0x800003eb,-(%sp)
 move.l %sp,%d1
 jsr -498(%a6)
 lea 28(%sp),%sp
 move.l (%sp)+,%a6
 rts
rtg_close:
 move.l 4,%a6
 move.l %d4,%a1
 jsr -414(%a6)
rtg_free_icon:
 move.l %d6,%a6
 move.l %d5,%a0
 jsr -90(%a6)
rtg_close_icon:
 move.l 4,%a6
 move.l %d6,%a1
 jsr -414(%a6)
rtg_close_dos:
 move.l 4,%a6
 move.l %d7,%a1
 jsr -414(%a6)
rtg_restore:
 move.l (%sp)+,184(%a4)
rtg_done:
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
/* the board's display ID */
.equ WB_MODE, 0x50001000
.balign 4
/* While Workbench stays on another mode, try to reopen it every two
 * seconds; it closes only once no other windows are left on it. */
wb_watch:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 clr.l -(%sp)
 move.l 4,%a6
 lea dos_name,%a1
 moveq #36,%d0
 jsr -552(%a6)
 move.l %d0,%a3
 lea intuition_name,%a1
 moveq #39,%d0
 jsr -552(%a6)
 move.l %d0,%a5
 lea graphics_name,%a1
 moveq #39,%d0
 jsr -552(%a6)
 move.l %d0,%a4
 tst.l %d0
 beq watch_close
 move.l %a3,%d0
 beq watch_close
 move.l %a5,%d0
 beq watch_close
 moveq #0,%d4
 moveq #0,%d5
 move.w #479,%d6
watch_poll:
 move.l %a3,%a6
 moveq #25,%d1
 jsr -198(%a6)
 /* walk the screen list: looking a public screen up would open Workbench */
 move.l %a5,%a6
 moveq #0,%d0
 jsr -414(%a6)
 move.l %d0,%d2
 moveq #-1,%d3
 move.l 60(%a5),%a2
1: move.l %a2,%d0
 beq 2f
 move.w 20(%a2),%d0
 and.w #15,%d0
 cmp.w #1,%d0
 beq 3f
 move.l (%a2),%a2
 bra 1b
3: lea 44(%a2),%a0
 move.l %a4,%a6
 jsr -792(%a6)
 move.l %d0,%d3
2: move.l %a5,%a6
 move.l %d2,%a0
 jsr -420(%a6)
 cmp.l #-1,%d3
 beq watch_next
 cmp.l #WB_MODE,%d3
 beq watch_close
 addq.l #1,%d4
 cmp.l #4,%d4
 blo watch_next
 cmp.l #60,%d5
 bhs watch_close
 cmp.l #2,(%sp)
 bhs watch_close
 addq.l #1,%d5
 moveq #0,%d4
 jsr -78(%a6)
 tst.l %d0
 beq 1f
 addq.l #1,(%sp)
1: jsr -210(%a6)
watch_next:
 dbra %d6,watch_poll
watch_close:
 move.l 4,%a6
 move.l %a4,%a1
 jsr -414(%a6)
 move.l %a5,%a1
 jsr -414(%a6)
 move.l %a3,%a1
 jsr -414(%a6)
 addq.l #4,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 moveq #0,%d0
 rts
.balign 4
handler_image:
 .incbin "handler-image.bin"
handler_relocations:
 .include "handler-relocs.inc"
.balign 4
extension_end:
