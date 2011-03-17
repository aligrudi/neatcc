@ ARM software division implementation
@ These functions are assembled using neatas and are included in gen.c
.global __udivdi3
__udivdi3:
	mov	r2, #0
	mov	r3, #0

	@ zero divider
	tst	r1, r1
	beq	.end

	@ shift the operand
.shl:
	movs	r12, r1, LSL r2
	add	r2, r2, #1
	bpl	.shl

	mov	r12, #1

	@ the main division algorithm
.shr:
	subs	r2, r2, #1
	bmi	.end
	cmps	r0, r1, LSL r2
	bcc	.shr
	sub	r0, r0, r1, LSL r2
	add	r3, r3, r12, LSL r2
	b	.shr
.end:
	mov	r1, r0
	mov	r0, r3
	mov	pc, lr

.global __umoddi3
__umoddi3:
	stmfd	sp!, {lr}
	bl	__udivdi3
	mov	r0, r1
	ldmfd	sp!, {pc}

.global __divdi3
__divdi3:
	stmfd	sp!, {r4, r5, lr}

	mov	r4, r0
	mov	r5, r1

	@ handle negative operands
	tst	r0, r0
	rsbmi	r0, r0, #0
	tst	r1, r1
	rsbmi	r1, r1, #0

	bl	__udivdi3

	@ result is negative
	teq	r4, r5
	rsbmi	r0, r0, #0
	tst	r4, r4
	rsbmi	r1, r1, #0

	ldmfd	sp!, {r4, r5, pc}

.global __moddi3
__moddi3:
	stmfd	sp!, {lr}
	bl	__divdi3
	mov	r0, r1
	ldmfd	sp!, {pc}
