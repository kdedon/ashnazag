	.text
	.globl entry, auxsysr, cfmcall, exitapp
entry:
	pea -4(%a5)
	.short 0xa86e
	.short 0xa8fe
	.short 0xa912
	.short 0xa930
	.short 0xa9cc
	clrl -(%sp)
	.short 0xa97b
	bra start
auxsysr:
	movel 4(%sp),%d0
	movel 16(%sp),-(%sp)
	movel 16(%sp),-(%sp)
	movel 16(%sp),-(%sp)
	clrl -(%sp)
	trap #0
	lea 16(%sp),%sp
	bccs 1f
	negl %d0
1:	rts
cfmcall:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a0
	movel 52(%sp),%d1
	movel 56(%sp),%d0
	clrw -(%sp)
1:	tstl %d1
	beqs 2f
	movel (%a0)+,-(%sp)
	subql #1,%d1
	bras 1b
2:	movew %d0,-(%sp)
	.short 0xaa5a
	movew (%sp)+,%d0
	extl %d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.globl newdescriptor, newnativecallback, disposedescriptor, vector4, vector9
vector4:
	movel 0x10,%d0
	rts
vector9:
	movel 0x24,%d0
	rts
newdescriptor:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a0
	movel 52(%sp),%a1
	clrl -(%sp)
	movel %a0,-(%sp)
	movel %a1,-(%sp)
| The trap reads the ISA byte at the top of its padded stack slot.
	movew #0x1000,-(%sp)
	moveq #0,%d0
	.short 0xaa59
	movel (%sp)+,%d0
	movel %d0,%a0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
newnativecallback:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a0
	movel 52(%sp),%a1
	clrl -(%sp)
	movel %a0,-(%sp)
	movel %a1,-(%sp)
| The trap reads the ISA byte at the top of its padded stack slot.
	clrw -(%sp)
	moveq #0,%d0
	.short 0xaa59
	movel (%sp)+,%d0
	movel %d0,%a0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
disposedescriptor:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),-(%sp)
	moveq #1,%d0
	.short 0xaa59
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.globl descriptorcall
descriptorcall:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a0
	jsr (%a0)
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.globl descriptorcalln
descriptorcalln:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	link %a6,#0
	movel 52(%a6),%a0
	movel 56(%a6),%a1
	movel 60(%a6),%d1
	lsll #2,%d1
	addl %d1,%a1
1:	tstl %d1
	beqs 2f
	movel -(%a1),-(%sp)
	subql #4,%d1
	bras 1b
2:	jsr (%a0)
	unlk %a6
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
exitapp:
	.short 0xa9f4
	rts
	.section .note.GNU-stack,"",@progbits

    .text
    .globl loadword
loadword:
    moveal 4(%sp),%a0
    movel (%a0),%d0
    rts

	.text
	.globl notify_entry, currenta5, tickcount, systask
currenta5:
	movel %a5,%d0
	rts
tickcount:
	movel 0x16a,%d0
	rts
systask:
	.short 0xa9b4
	rts
notify_entry:
	link %a6,#0
	moveml %d2-%d7/%a2-%a5,-(%sp)
	movel 20(%a6),%a0
	movel #1,44(%a0)
	movel (%a0),%a5
	movel 8(%a6),-(%sp)
	movel 12(%a6),-(%sp)
	movel 16(%a6),-(%sp)
	movel 20(%a6),-(%sp)
	bsr notify_event
	lea 16(%sp),%sp
	movel 20(%a6),%a0
	movel #4,44(%a0)
	moveml (%sp)+,%d2-%d7/%a2-%a5
	unlk %a6
	rtd #16

	.globl notify_cfm_entry
notify_cfm_entry:
	movel (%a1),%a5
	link %a6,#-8
	moveml %d2-%d7/%a2-%a5,-(%sp)
	movel 8(%a6),%a0
	movel #1,44(%a0)
	movel 20(%a6),-(%sp)
	movel 16(%a6),-(%sp)
	movel 12(%a6),-(%sp)
	movel 8(%a6),-(%sp)
	bsr notify_event
	lea 16(%sp),%sp
	movel 8(%a6),%a0
	movel #4,44(%a0)
	moveml (%sp)+,%d2-%d7/%a2-%a5
	unlk %a6
	rtd #16

	.text
	.globl findextensions, hopenresource, resourcecount, namedresource
	.globl resourceref, resourceerror, useresource
	.macro resource_enter
	moveml %d2-%d7/%a2-%a6,-(%sp)
	link %a6,#0
	.endm
	.macro resource_word_return
	movew (%sp)+,%d0
	extl %d0
	unlk %a6
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.endm
findextensions:
	resource_enter
	clrw -(%sp)
	movew #-32768,-(%sp)
	movel #0x6578746e,-(%sp)
	clrw -(%sp)
	movel 52(%a6),-(%sp)
	movel 56(%a6),-(%sp)
	moveq #0,%d0
	.short 0xa823
	resource_word_return
hopenresource:
	resource_enter
	clrw -(%sp)
	movew 54(%a6),-(%sp)
	movel 56(%a6),-(%sp)
	movel 60(%a6),-(%sp)
	movew #0x0100,-(%sp)
	.short 0xa81a
	resource_word_return
resourcecount:
	resource_enter
	clrw -(%sp)
	movel 52(%a6),-(%sp)
	.short 0xa80d
	resource_word_return
namedresource:
	resource_enter
	clrl -(%sp)
	movel 52(%a6),-(%sp)
	movel 56(%a6),-(%sp)
	.short 0xa820
	movel (%sp)+,%d0
	unlk %a6
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
resourceref:
	resource_enter
	clrw -(%sp)
	.short 0xa994
	resource_word_return
resourceerror:
	resource_enter
	clrw -(%sp)
	.short 0xa9af
	resource_word_return
useresource:
	resource_enter
	movew 54(%a6),-(%sp)
	.short 0xa998
	moveq #0,%d0
	unlk %a6
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl hfileinfo
hfileinfo:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	movel 48(%sp),%a0
	.short 0xa20c
	extl %d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts

    .globl indexedresource, resourcesize
indexedresource:
    resource_enter
    clrl -(%sp)
    movel 52(%a6),-(%sp)
    movew 58(%a6),-(%sp)
    .short 0xa80e
    movel (%sp)+,%d0
    unlk %a6
    moveml (%sp)+,%d2-%d7/%a2-%a6
    rts
resourcesize:
    resource_enter
    clrl -(%sp)
    movel 52(%a6),-(%sp)
    .short 0xa9a5
    movel (%sp)+,%d0
    unlk %a6
    moveml (%sp)+,%d2-%d7/%a2-%a6
    rts

    .text
    .globl notify_selfcall
notify_selfcall:
    moveml %d2-%d7/%a2-%a6,-(%sp)
    link %a6,#0
    movel 52(%a6),%a0
    movel 56(%a6),-(%sp)
    clrl -(%sp)
    clrl -(%sp)
    clrl -(%sp)
    jsr (%a0)
    unlk %a6
    moveml (%sp)+,%d2-%d7/%a2-%a6
    rts
