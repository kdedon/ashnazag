	.text
	.globl entry, auxwrite, auxsysr, hfs, create, openrf, fread, fwrite, fclose
	.globl getinfo, setinfo, renamefile, removefile, volinfo, exitapp
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
auxwrite:
	moveq #4,%d0
	trap #0
	rts
hfs:
	movel 4(%sp),%a0
	movel 8(%sp),%d0
	.short 0xa260
	extl %d0
	rts
	.macro pbcall name, trap
\name:
	movel 4(%sp),%a0
	.short \trap
	extl %d0
	rts
	.endm
	pbcall create, 0xa208
	pbcall openrf, 0xa20a
	pbcall fread, 0xa002
	pbcall fwrite, 0xa003
	pbcall fclose, 0xa001
	pbcall getinfo, 0xa20c
	pbcall setinfo, 0xa20d
	pbcall renamefile, 0xa20b
	pbcall removefile, 0xa209
	pbcall volinfo, 0xa207
exitapp:
	.short 0xa9f4
	rts

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

	.globl getnext, getprocess
getnext:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	movew #56,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	extl %d0
	rts
getprocess:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	movel 14(%sp),-(%sp)
	movew #58,-(%sp)
	.short 0xa88f
	movew (%sp)+,%d0
	extl %d0
	rts

| Manager calls take an array of long Pascal arguments.
	.globl cfmcall, dragcall, aliascall, threadcall, threadentry, disposehandle
	.macro manager name, trap, stacksel
\name:
	movel 4(%sp),%a0
	movel 8(%sp),%d1
	movel 12(%sp),%d0
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrw -(%sp)
1:	tstl %d1
	beqs 2f
	movel (%a0)+,-(%sp)
	subql #1,%d1
	bras 1b
2:
	.if \stacksel
	movew %d0,-(%sp)
	.endif
	.short \trap
	movew (%sp)+,%d0
	extl %d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.endm
	manager cfmcall, 0xaa5a, 1
	manager dragcall, 0xabed, 0
	manager threadcall, 0xabf2, 0
	manager aliascall, 0xa823, 0
	.globl aecall
aecall:
	movel 4(%sp),%a0
	movel 8(%sp),%d1
	movel 12(%sp),%d0
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrw -(%sp)
1:	tstl %d1
	beqs 2f
	movew (%a0)+,-(%sp)
	subql #1,%d1
	bras 1b
2:	.short 0xa816
	movew (%sp)+,%d0
	extl %d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
disposehandle:
	movel 4(%sp),%a0
	.short 0xa023
	rts

	.globl threaddispose
threaddispose:
	clrw -(%sp)
	movel 6(%sp),-(%sp)
	clrl -(%sp)
	clrw -(%sp)
	movew #0x0504,%d0
	.short 0xabf2
	movew (%sp)+,%d0
	extl %d0
	rts
threadentry:
	moveal 4(%sp),%a0
	movel #0x81cafe,(%a0)
	movel %a0,8(%sp)
	moveal (%sp)+,%a1
	addql #4,%sp
	jmp (%a1)
	.globl videolist, videomain, videonext
videolist:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrl -(%sp)
	.short 0xaa29
	movel (%sp)+,%d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
videomain:
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrl -(%sp)
	.short 0xaa2a
	movel (%sp)+,%d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
videonext:
	movel 4(%sp),%a0
	moveml %d2-%d7/%a2-%a6,-(%sp)
	clrl -(%sp)
	movel %a0,-(%sp)
	.short 0xaa2b
	movel (%sp)+,%d0
	moveml (%sp)+,%d2-%d7/%a2-%a6
	rts
	.section .note.GNU-stack,"",@progbits
