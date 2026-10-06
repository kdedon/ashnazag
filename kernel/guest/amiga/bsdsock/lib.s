| bsdsocket.library: resident tag, vectors and register glue for the C side.
| Library calls pass arguments in registers; the C functions take them on
| the stack, last the caller's library base.

 .text
 .globl _start
_start:
 moveq #-1,%d0
 rts

romtag:
 .word 0x4afc
 .long romtag, endskip
 .byte 0x80, 4, 9, 0		| RTF_AUTOINIT, version, NT_LIBRARY, priority
 .long bs_name, bs_idstring, bs_autoinit

| fn, argument count, registers last argument first
 .macro lvo fn, n, regs:vararg
 move.l %a6,-(%sp)
 .irp r,\regs
 move.l \r,-(%sp)
 .endr
 jsr \fn
 lea (4+4*\n)(%sp),%sp
 rts
 .endm
 .macro lvo0 fn
 move.l %a6,-(%sp)
 jsr \fn
 addq.l #4,%sp
 rts
 .endm

 .globl linit
linit:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l %a0,-(%sp)
 move.l %d0,-(%sp)
 jsr bs_init
 addq.l #8,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 rts
lopen: lvo0 bs_open
lclose: lvo0 bs_close
lexpunge: lvo0 bs_expunge
lnull:
 moveq #0,%d0
 rts

l_socket: lvo bs_socket, 3, %d2, %d1, %d0
l_bind: lvo bs_bind, 3, %d1, %a0, %d0
l_listen: lvo bs_listen, 2, %d1, %d0
l_accept: lvo bs_accept, 3, %a1, %a0, %d0
l_connect: lvo bs_connect, 3, %d1, %a0, %d0
l_sendto: lvo bs_sendto, 6, %d3, %a1, %d2, %d1, %a0, %d0
l_send: lvo bs_send, 4, %d2, %d1, %a0, %d0
l_recvfrom: lvo bs_recvfrom, 6, %a2, %a1, %d2, %d1, %a0, %d0
l_recv: lvo bs_recv, 4, %d2, %d1, %a0, %d0
l_shutdown: lvo bs_shutdown, 2, %d1, %d0
l_setsockopt: lvo bs_setsockopt, 5, %d3, %a0, %d2, %d1, %d0
l_getsockopt: lvo bs_getsockopt, 5, %a1, %a0, %d2, %d1, %d0
l_getsockname: lvo bs_getsockname, 3, %a1, %a0, %d0
l_getpeername: lvo bs_getpeername, 3, %a1, %a0, %d0
l_ioctl: lvo bs_ioctl, 3, %a0, %d1, %d0
l_close: lvo bs_closesocket, 1, %d0
l_waitselect: lvo bs_waitselect, 6, %d1, %a3, %a2, %a1, %a0, %d0
l_setsignals: lvo bs_setsignals, 3, %d2, %d1, %d0
l_dtablesize: lvo0 bs_dtablesize
l_obtain: lvo bs_obtain, 4, %d3, %d2, %d1, %d0
l_release: lvo bs_release, 2, %d1, %d0
l_releasecopy: lvo bs_releasecopy, 2, %d1, %d0
l_errno: lvo0 bs_errno
l_seterrnoptr: lvo bs_seterrnoptr, 2, %d0, %a0
l_ntoa: lvo bs_ntoa, 1, %d0
l_inet_addr: lvo bs_inet_addr, 1, %a0
l_lnaof: lvo bs_lnaof, 1, %d0
l_netof: lvo bs_netof, 1, %d0
l_makeaddr: lvo bs_makeaddr, 2, %d1, %d0
l_inet_network: lvo bs_inet_network, 1, %a0
l_ghbyname: lvo bs_gethostbyname, 1, %a0
l_ghbyaddr: lvo bs_gethostbyaddr, 3, %d1, %d0, %a0
l_gnbyname: lvo bs_none, 1, %a0
l_gnbyaddr: lvo bs_none, 1, %d0
l_gsbyname: lvo bs_getservbyname, 2, %a1, %a0
l_gsbyport: lvo bs_getservbyport, 2, %a0, %d0
l_gpbyname: lvo bs_getprotobyname, 1, %a0
l_gpbynumber: lvo bs_getprotobynumber, 1, %d0
l_vsyslog:
 moveq #0,%d0
 rts
l_dup2: lvo bs_dup2, 2, %d1, %d0
l_sendmsg: lvo bs_sendmsg, 3, %d1, %a0, %d0
l_recvmsg: lvo bs_recvmsg, 3, %d1, %a0, %d0
l_gethostname: lvo bs_gethostname, 2, %d0, %a0
l_gethostid: lvo0 bs_gethostid
l_tags: lvo bs_tags, 1, %a0
l_events: lvo bs_events, 1, %a0

 .balign 4
 .globl vectors
vectors:
 .long lopen, lclose, lexpunge, lnull
 .long l_socket, l_bind, l_listen, l_accept, l_connect, l_sendto, l_send
 .long l_recvfrom, l_recv, l_shutdown, l_setsockopt, l_getsockopt
 .long l_getsockname, l_getpeername, l_ioctl, l_close, l_waitselect
 .long l_setsignals, l_dtablesize, l_obtain, l_release, l_releasecopy
 .long l_errno, l_seterrnoptr, l_ntoa, l_inet_addr, l_lnaof, l_netof
 .long l_makeaddr, l_inet_network, l_ghbyname, l_ghbyaddr, l_gnbyname
 .long l_gnbyaddr, l_gsbyname, l_gsbyport, l_gpbyname, l_gpbynumber
 .long l_vsyslog, l_dup2, l_sendmsg, l_recvmsg, l_gethostname
 .long l_gethostid, l_tags, l_events
 .long -1
endskip:

| long excall(lvo, d0, d1, a0, a1): exec.library
 .globl excall
excall:
 movem.l %d2/%a2/%a6,-(%sp)
 move.l 4.w,%a6
 move.l 16(%sp),%d2
 move.l 20(%sp),%d0
 move.l 24(%sp),%d1
 move.l 28(%sp),%a0
 move.l 32(%sp),%a1
 jsr 0(%a6,%d2.l)
 movem.l (%sp)+,%d2/%a2/%a6
 rts

| long hsys(n, a, b, c, d): a host system call; the result or -errno
 .globl hsys
hsys:
 move.l 20(%sp),-(%sp)
 move.l 20(%sp),-(%sp)
 move.l 20(%sp),-(%sp)
 move.l 20(%sp),-(%sp)
 clr.l -(%sp)
1: move.l 24(%sp),%d0
 trap #0
 bcc.s 2f
 cmp.l #4,%d0			| EINTR: a virtual interrupt arrived
 beq.s 1b
 neg.l %d0
2: lea 20(%sp),%sp
 rts

| PORTS server: wakes tasks waiting for host sockets; the chain goes on
 .globl bs_isr
bs_isr:
 movem.l %d2-%d7/%a2-%a6,-(%sp)
 move.l %a1,-(%sp)
 jsr bs_server
 addq.l #4,%sp
 movem.l (%sp)+,%d2-%d7/%a2-%a6
 lea 0xdff000,%a0
 moveq #0,%d0
 rts

 .section .note.GNU-stack,"",@progbits
