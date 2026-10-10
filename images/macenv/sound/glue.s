| SoundOut: the INIT's entry, the Sound Manager traps' Pascal glue and
| calls out of C.  Position-independent: the entry copies everything to
| a system-heap block and installs from there.
	.text
	.globl	entry
entry:
	moveml	%d3-%d7/%a2-%a6,-(%sp)
	lea	_end(%pc),%a0
	lea	entry(%pc),%a1
	movel	%a0,%d3
	subl	%a1,%d3
	movel	%d3,%d0
	.short	0xa71e			| _NewPtrSysClear
	tstl	%d0
	bnes	1f
	moveal	%a0,%a2
	lea	entry(%pc),%a0
	moveal	%a2,%a1
	movel	%d3,%d0
	.short	0xa02e			| _BlockMove
	.short	0xa0bd			| _FlushCodeCache
	lea	sm_install(%pc),%a0
	lea	entry(%pc),%a1
	subl	%a1,%a0
	addl	%a2,%a0
	jsr	(%a0)
1:	moveml	(%sp)+,%d3-%d7/%a2-%a6
	rts

| Pascal entries: arguments to C, the result to its slot, parameters popped.
| SndDoCommand(chan, cmd, noWait): OSErr
	.globl	p_docommand
p_docommand:
	movel	6(%sp),-(%sp)
	movel	14(%sp),-(%sp)
	bsrl	sm_docommand
	addql	#8,%sp
	movew	%d0,14(%sp)
	moveal	(%sp)+,%a0
	lea	10(%sp),%sp
	jmp	(%a0)

| SndDoImmediate(chan, cmd): OSErr
	.globl	p_doimmediate
p_doimmediate:
	movel	4(%sp),-(%sp)
	movel	12(%sp),-(%sp)
	bsrl	sm_docommand
	addql	#8,%sp
	movew	%d0,12(%sp)
	moveal	(%sp)+,%a0
	addql	#8,%sp
	jmp	(%a0)

| SndPlay(chan, sndHdl, async): OSErr
	.globl	p_play
p_play:
	movew	4(%sp),%d0
	extl	%d0
	movel	%d0,-(%sp)
	movel	10(%sp),-(%sp)
	movel	18(%sp),-(%sp)
	bsrl	sm_play
	lea	12(%sp),%sp
	movew	%d0,14(%sp)
	moveal	(%sp)+,%a0
	lea	10(%sp),%sp
	jmp	(%a0)

| SndNewChannel(var chan, synth, init, userRoutine): OSErr
	.globl	p_newchannel
p_newchannel:
	movel	4(%sp),-(%sp)
	movel	18(%sp),-(%sp)
	bsrl	sm_newchannel
	addql	#8,%sp
	movew	%d0,18(%sp)
	moveal	(%sp)+,%a0
	lea	14(%sp),%sp
	jmp	(%a0)

| SndDisposeChannel(chan, quietNow): OSErr
	.globl	p_disposechannel
p_disposechannel:
	movew	4(%sp),%d0
	extl	%d0
	movel	%d0,-(%sp)
	movel	10(%sp),-(%sp)
	bsrl	sm_disposechannel
	addql	#8,%sp
	movew	%d0,10(%sp)
	moveal	(%sp)+,%a0
	addql	#6,%sp
	jmp	(%a0)

| SndAddModifier(chan, modifier, id, init) and SndControl(id, var cmd): noErr
	.globl	p_addmodifier
p_addmodifier:
	moveal	(%sp)+,%a0
	lea	14(%sp),%sp
	clrw	(%sp)
	jmp	(%a0)
	.globl	p_control
p_control:
	moveal	(%sp)+,%a0
	addql	#6,%sp
	clrw	(%sp)
	jmp	(%a0)

| _SoundDispatch: parameter words in d0's high byte.  No parameters: a
| version (2.0 for the Sound Manager, else 0); MACE calls and
| SndGetSysBeepState return nothing; sound input siNoSoundInHardware;
| the rest notEnoughHardwareErr.
	.globl	p_dispatch
p_dispatch:
	moveal	(%sp)+,%a0
	movel	%d0,%d1
	roll	#8,%d1
	andiw	#0xff,%d1
	addw	%d1,%d1
	addaw	%d1,%sp
	tstw	%d1
	bnes	2f
	clrl	(%sp)
	cmpl	#0x000c0008,%d0
	bnes	1f
	movel	#0x02000000,(%sp)
1:	jmp	(%a0)
2:	cmpiw	#0x0010,%d0
	beqs	1b
	cmpil	#0x02180008,%d0
	beqs	1b
	movew	#-201,(%sp)
	cmpiw	#0x0014,%d0
	bnes	1b
	movew	#-220,(%sp)
	jmp	(%a0)

| Unix calls: auxsys(n, a, b, c) -> d0, -1 on error
	.globl	auxsys
auxsys:
	movel	4(%sp),%d0
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	clrl	-(%sp)
	trap	#0
	lea	16(%sp),%sp
	bccs	1f
	moveq	#-1,%d0
1:	rts

| settrap(trap, addr): _SetToolTrapAddress
	.globl	settrap
settrap:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	movel	48(%sp),%d0
	moveal	52(%sp),%a0
	.short	0xa647
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| newptr(size) -> system-heap block, cleared; 0 if none
	.globl	newptr
newptr:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	movel	48(%sp),%d0
	.short	0xa71e
	tstl	%d0
	beqs	1f
	subal	%a0,%a0
1:	movel	%a0,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	disposeptr
disposeptr:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	.short	0xa01f
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| callback(proc, chan, cmd): a Pascal procedure (chan; var cmd)
	.globl	callback
callback:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	moveal	56(%sp),%a0
	jsr	(%a0)
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| hlock(h): state, then _HLock
	.globl	hlock
hlock:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	.short	0xa069			| _HGetState
	movel	%d0,-(%sp)
	moveal	52(%sp),%a0
	.short	0xa029			| _HLock
	movel	(%sp)+,%d0
	andl	#0xff,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| hsetstate(h, state)
	.globl	hsetstate
hsetstate:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	movel	52(%sp),%d0
	.short	0xa06a			| _HSetState
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| auxsysr(n, a, b, c) -> d0, or -errno
	.globl	auxsysr
auxsysr:
	movel	4(%sp),%d0
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	movel	16(%sp),-(%sp)
	clrl	-(%sp)
	trap	#0
	lea	16(%sp),%sp
	bccs	1f
	negl	%d0
1:	rts

| gettrap(trap): _GetToolTrapAddress
	.globl	gettrap
gettrap:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	movel	48(%sp),%d0
	.short	0xa746
	movel	%a0,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| newhandlesys(size) -> handle in the system heap, cleared; 0 if none
	.globl	newhandlesys
newhandlesys:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	movel	48(%sp),%d0
	.short	0xa722
	tstw	%d0
	beqs	1f
	subal	%a0,%a0
1:	movel	%a0,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

	.globl	disposehandle
disposehandle:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	.short	0xa023
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| ticks(): Ticks
	.globl	ticks
ticks:
	movel	0x16a:w,%d0
	rts

| splhi() -> old sr; splx(sr)
	.globl	splhi
splhi:
	movew	%sr,%d0
	oriw	#0x0700,%sr
	rts
	.globl	splx
splx:
	movew	6(%sp),%sr
	rts

| instime(task), primetime(task, ms), dtinstall(task)
	.globl	instime
instime:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	.short	0xa058
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts
	.globl	primetime
primetime:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	movel	52(%sp),%d0
	.short	0xa05a
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts
	.globl	dtinstall
dtinstall:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	moveal	48(%sp),%a0
	.short	0xa082
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| Time Manager and deferred task entries; A1 is the task
	.globl	t_tm
t_tm:
	moveml	%d0-%d2/%a0-%a1,-(%sp)
	bsrl	tm_run
	moveml	(%sp)+,%d0-%d2/%a0-%a1
	rts
	.globl	t_dt
t_dt:
	moveml	%d0-%d2/%a0-%a1,-(%sp)
	bsrl	dt_run
	moveml	(%sp)+,%d0-%d2/%a0-%a1
	rts

| The output component's entry: (params, storage): ComponentResult
	.globl	p_sdev
p_sdev:
	movel	4(%sp),-(%sp)
	movel	12(%sp),-(%sp)
	bsrl	sdev
	addql	#8,%sp
	moveal	(%sp)+,%a0
	addql	#8,%sp
	movel	%d0,(%sp)
	jmp	(%a0)

| delegate(params, ci): DelegateComponentCall
	.globl	delegate
delegate:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	moveq	#0x24,%d0
	.short	0xa82a
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| registercomp(desc, entry, name): RegisterComponent, global
	.globl	registercomp
registercomp:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	movew	#1,-(%sp)
	movel	70(%sp),-(%sp)
	clrl	-(%sp)
	clrl	-(%sp)
	moveq	#0x01,%d0
	.short	0xa82a
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| findnext(comp, desc): FindNextComponent
	.globl	findnext
findnext:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	moveq	#0x04,%d0
	.short	0xa82a
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| capture(comp, by): CaptureComponent
	.globl	capture
capture:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	moveq	#0x1c,%d0
	.short	0xa82a
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| setdefault(comp, flags): SetDefaultComponent
	.globl	setdefault
setdefault:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movew	60(%sp),-(%sp)
	moveq	#0x1e,%d0
	.short	0xa82a
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| getsrc(ci, &data): SoundComponentGetSourceData
	.globl	getsrc
getsrc:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	52(%sp),-(%sp)
	movel	60(%sp),-(%sp)
	movel	#0x00040004,-(%sp)
	moveq	#0,%d0
	.short	0xa82a
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| openmixer(desc, flags, &ci): OpenMixerSoundComponent
	.globl	openmixer
openmixer:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movel	58(%sp),-(%sp)
	movel	66(%sp),-(%sp)
	movel	#0x06140018,%d0
	.short	0xa800
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| closemixer(ci): CloseMixerSoundComponent
	.globl	closemixer
closemixer:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movel	#0x02180018,%d0
	.short	0xa800
	movew	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| smversion(): SndSoundManagerVersion
	.globl	smversion
smversion:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrl	-(%sp)
	movel	#0x000c0008,%d0
	.short	0xa800
	movel	(%sp)+,%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| Sound Manager calls for the self-test, through the trap table
| sndnew(&chan, synth, init, proc)
	.globl	sndnew
sndnew:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movew	60(%sp),-(%sp)
	movel	64(%sp),-(%sp)
	movel	72(%sp),-(%sp)
	.short	0xa807
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| sndplay(chan, h, async)
	.globl	sndplay
sndplay:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movel	58(%sp),-(%sp)
	movew	68(%sp),%d0
	lslw	#8,%d0
	movew	%d0,-(%sp)
	.short	0xa805
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| snddo(chan, cmd): SndDoCommand, waiting for room
	.globl	snddo
snddo:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movel	58(%sp),-(%sp)
	clrw	-(%sp)
	.short	0xa803
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| snddispose(chan): SndDisposeChannel, quiet
	.globl	snddispose
snddispose:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
	clrw	-(%sp)
	movel	50(%sp),-(%sp)
	movew	#0x0100,-(%sp)
	.short	0xa801
	movew	(%sp)+,%d0
	extl	%d0
	moveml	(%sp)+,%d2-%d7/%a2-%a6
	rts

| the self-test's callBackCmd routine: (chan, var cmd)
	.globl	t_cb
t_cb:
	movel	%a0,-(%sp)
	moveal	8(%sp),%a0
	movel	4(%a0),-(%sp)
	bsrl	cb_run
	addql	#4,%sp
	moveal	(%sp)+,%a0
	moveal	(%sp)+,%a1
	addql	#8,%sp
	jmp	(%a1)

| Task-level component and SoundDispatch observations.
    .globl probe_opencomp
probe_opencomp:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
    clrl -(%sp)
    movel 52(%sp),-(%sp)
    moveq #7,%d0
    .short 0xa82a
    movel (%sp)+,%d0
    moveml	(%sp)+,%d2-%d7/%a2-%a6
    rts
    .globl probe_closecomp
probe_closecomp:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
    clrw -(%sp)
    movel 50(%sp),-(%sp)
    moveq #8,%d0
    .short 0xa82a
    movew (%sp)+,%d0
    extl %d0
    moveml	(%sp)+,%d2-%d7/%a2-%a6
    rts
    .globl probe_soundtool
probe_soundtool:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
    clrl -(%sp)
    movew 54(%sp),-(%sp)
    movel #0x000c0000,%d0
    .short 0xa800
    movel (%sp)+,%d0
    moveml	(%sp)+,%d2-%d7/%a2-%a6
    rts
    .globl probe_gestalt
probe_gestalt:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
    movel 48(%sp),%d0
    .short 0xa1ad
    moveal 52(%sp),%a1
    movel %a0,(%a1)
    extl %d0
    moveml	(%sp)+,%d2-%d7/%a2-%a6
    rts

| Native GetDefaultSoundOutput(Component *).
    .globl probe_defaultout
probe_defaultout:
	moveml	%d2-%d7/%a2-%a6,-(%sp)
    clrw -(%sp)
    movel 50(%sp),-(%sp)
    movel #0x021c0018,%d0
    .short 0xa800
    movew (%sp)+,%d0
    extl %d0
    moveml	(%sp)+,%d2-%d7/%a2-%a6
    rts

    .globl probe_memtop
probe_memtop:
    movel 0x108,%d0
    rts

    .globl probe_rombase
probe_rombase:
    movel 0x2ae,%d0
    rts
