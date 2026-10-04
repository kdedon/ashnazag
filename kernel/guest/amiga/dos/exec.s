.text
.globl _start
_start:
 jsr handler
 moveq #0,%d0
 rts
.globl allocmem
allocmem:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%d0
 move.l #65537,%d1
 jsr -198(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl freemem
freemem:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a1
 move.l 12(%sp),%d0
 jsr -210(%a6)
 move.l (%sp)+,%a6
 rts
.globl findtask
findtask:
 move.l %a6,-(%sp)
 move.l 4,%a6
 sub.l %a1,%a1
 jsr -294(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl waitport
waitport:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 jsr -384(%a6)
 move.l (%sp)+,%a6
 rts
.globl getmsg
getmsg:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 jsr -372(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl putmsg
putmsg:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 move.l 12(%sp),%a1
 jsr -366(%a6)
 move.l (%sp)+,%a6
 rts
.globl createport
createport:
 move.l %a6,-(%sp)
 move.l 4,%a6
 nop
 jsr -666(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl createio
createio:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 move.l #40,%d0
 jsr -654(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl opendevice
opendevice:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 move.l 12(%sp),%a1
 moveq #0,%d0
 moveq #0,%d1
 jsr -444(%a6)
 move.l (%sp)+,%a6
 rts
.globl doio
doio:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a1
 jsr -456(%a6)
 move.l (%sp)+,%a6
 rts
.globl deleteio
deleteio:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 jsr -660(%a6)
 move.l (%sp)+,%a6
 rts
.globl deleteport
deleteport:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a0
 jsr -672(%a6)
 move.l (%sp)+,%a6
 rts
.globl closedevice
closedevice:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a1
 jsr -450(%a6)
 move.l (%sp)+,%a6
 rts
.globl forbid
forbid:
 move.l %a6,-(%sp)
 move.l 4,%a6
 jsr -132(%a6)
 move.l (%sp)+,%a6
 rts
.globl permit
permit:
 move.l %a6,-(%sp)
 move.l 4,%a6
 jsr -138(%a6)
 move.l (%sp)+,%a6
 rts
.globl openlibrary
openlibrary:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a1
 moveq #36,%d0
 jsr -552(%a6)
 move.l %d0,%a0
 move.l (%sp)+,%a6
 rts
.globl closelibrary
closelibrary:
 move.l %a6,-(%sp)
 move.l 4,%a6
 move.l 8(%sp),%a1
 jsr -414(%a6)
 move.l (%sp)+,%a6
 rts
.globl adddosentry
adddosentry:
 move.l %a6,-(%sp)
 move.l 8(%sp),%a6
 move.l 12(%sp),%d1
 jsr -678(%a6)
 move.l (%sp)+,%a6
 rts
.section .note.GNU-stack,"",@progbits
