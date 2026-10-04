| hat_ptalloc for callers that wait without stealing (HAT_CANWAIT|HAT_NOSTEAL).
| The stock allocator sleeps on free_pts, which only a page-table free wakes,
| so a table request that meets an empty free list could sleep for good once
| the page daemon has restored freemem.  Here an empty free list wakes the
| page daemon and waits on freemem, as page_get does; any other shortage
| (memory accounting, ptdat space) retries every second.
	.text
	.globl	hat_ptalloc
hat_ptalloc:
	movel	%sp@(8),%d0
	andil	&3,%d0
	cmpil	&3,%d0
	beqw	Lpw_wait
	jmp	__amix_hat_ptalloc
Lpw_wait:
	moveml	%d2/%a2,%sp@-
	moveal	%sp@(12),%a2		| ptdat out-parameter
Lpw_try:
	pea	2			| HAT_NOSTEAL alone: fail instead of sleeping
	movel	%a2,%sp@-
	jsr	__amix_hat_ptalloc
	addqw	&8,%sp
	tstl	%d0
	bnew	Lpw_done
	movew	%sr,%d2
	movew	&0x2400,%sr		| freemem test and sleep without a lost wakeup
	tstl	freemem
	bgtw	Lpw_other
	pea	1
	movel	proc_pageout,%sp@-
	jsr	wakeprocs
	addqw	&8,%sp
	addql	&1,freemem_wait
	pea	2
	pea	freemem
	jsr	sleep
	addqw	&8,%sp
	braw	Lpw_spl
Lpw_other:
	pea	60			| one second
	clrl	%sp@-
	pea	Lpw_tick
	jsr	timeout
	lea	%sp@(12),%sp
	movel	%d0,%sp@-		| timeout id, untimeout's argument
	addql	&1,pt_waiting
	pea	2
	pea	free_pts
	jsr	sleep
	addqw	&8,%sp
	jsr	untimeout
	addqw	&4,%sp
Lpw_spl:
	movew	%d2,%sr
	braw	Lpw_try
Lpw_done:
	moveal	%d0,%a0			| table address in both d0 and a0
	moveml	%sp@+,%d2/%a2
	rts

Lpw_tick:
	pea	1
	pea	free_pts
	jsr	wakeprocs
	addqw	&8,%sp
	rts
