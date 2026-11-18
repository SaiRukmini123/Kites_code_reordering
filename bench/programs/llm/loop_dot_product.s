# name: loop_dot_product
# category: loop
# source: llm
# inputs: none
# description: dot product of two 8-element vectors
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
a: .word 1, 2, 3, 4, 5, 6, 7, 8
b: .word 8, 7, 6, 5, 4, 3, 2, 1
res: .word 0
.text
la a0, a
la a1, b
li t3, 8
li t4, 0
loop:
lw t0, 0(a0)
lw t1, 0(a1)
mul t2, t0, t1
add t4, t4, t2
addi a0, a0, 4
addi a1, a1, 4
addi t3, t3, -1
bnez t3, loop
la a2, res
sw t4, 0(a2)
li a7, 10
ecall
