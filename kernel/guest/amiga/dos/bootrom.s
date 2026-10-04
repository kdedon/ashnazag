.include "handler-image.inc"
.text
.globl extension_resident
extension_resident:
 .word 0x4afc
 .long extension_resident,extension_payload_end
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
handler_image:
 .incbin "handler-image.bin"
handler_relocations:
 .include "handler-relocs.inc"
.balign 4
extension_end:
