.text
/* trackdisk.device: the ROM module's name, version and priority, so it takes the ROM's place */
.globl td_resident
td_resident:
 .word 0x4afc
 .long td_resident,td_end
 .byte 1,47,3,20
 .long td_name,td_id,td_initcode
.globl td_name, td_id
td_name: .asciz "trackdisk.device"
td_id: .asciz "trackdisk 47.1 (environment floppy service)"
.balign 4
.globl td_vectors
td_vectors:
 .long td_openv,td_closev,td_zero,td_zero,td_beginiov,td_abortiov,-1
td_initcode:
 jsr td_init
 rts
/* Open: d0 unit, a1 request, a6 device */
td_openv:
 move.l %d0,-(%sp)
 move.l %a1,-(%sp)
 move.l %a6,-(%sp)
 jsr td_open
 lea 12(%sp),%sp
 rts
td_closev:
 move.l %a1,-(%sp)
 move.l %a6,-(%sp)
 jsr td_close
 addq.l #8,%sp
 rts
td_zero:
 moveq #0,%d0
 rts
td_beginiov:
 move.l %a1,-(%sp)
 move.l %a6,-(%sp)
 jsr td_beginio
 addq.l #8,%sp
 rts
td_abortiov:
 move.l %a1,-(%sp)
 move.l %a6,-(%sp)
 jsr td_abortio
 addq.l #8,%sp
 moveq #0,%d0
 rts
td_end:
