	.text
	.globl	k68start
k68start:
	.long	k68int - k68start
	.long	k68copy - k68start
	.long	k68bytecopy - k68start
	.long	k68call - k68start
	.long	k68muldiv - k68start

k68int:
	move.l	4(%sp),%d1
	moveq	#0,%d0
	move.l	%d2,-(%sp)
	move.l	#0x12345678,%d2
1:	add.l	%d1,%d0
	eor.l	%d2,%d0
	rol.l	#3,%d0
	subq.l	#1,%d1
	bne.s	1b
	move.l	(%sp)+,%d2
	rts

k68copy:
	move.l	4(%sp),%a0
	move.l	8(%sp),%a1
	move.l	12(%sp),%d0
1:	move.l	(%a0)+,(%a1)+
	subq.l	#1,%d0
	bne.s	1b
	rts

k68bytecopy:
	move.l	4(%sp),%a0
	move.l	8(%sp),%a1
	move.l	12(%sp),%d0
1:	move.b	(%a0)+,(%a1)+
	subq.l	#1,%d0
	bne.s	1b
	rts

k68call:
	move.l	4(%sp),%d1
	moveq	#0,%d0
1:	bsr.s	2f
	subq.l	#1,%d1
	bne.s	1b
	rts
2:	addq.l	#1,%d0
	rts

k68muldiv:
	move.l	4(%sp),%d1
	moveq	#0,%d0
	move.l	%d2,-(%sp)
1:	moveq	#0,%d2
	move.w	%d1,%d2
	mulu.w	#5,%d2
	divu.w	#7,%d2
	add.w	%d2,%d0
	swap	%d2
	add.w	%d2,%d0
	subq.l	#1,%d1
	bne.s	1b
	move.l	(%sp)+,%d2
	rts
