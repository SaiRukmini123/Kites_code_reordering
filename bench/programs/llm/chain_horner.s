# name: chain_horner
# category: chain
# source: llm
# inputs: none
# description: Horner evaluation of a degree-5 polynomial
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
coef: .word 3, -2, 5, 7, -1, 4
x: .word 3
out: .word 0
.text
la a0, coef
lw t1, 24(a0)
li t3, 5
lw t2, 0(a0)
loop:
addi a0, a0, 4
mul t2, t2, t1
lw t0, 0(a0)
add t2, t2, t0
addi t3, t3, -1
bnez t3, loop
la a1, out
sw t2, 0(a1)
li a7, 10
ecall
