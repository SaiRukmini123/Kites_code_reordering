# name: mixed_transpose_3x3
# category: mixed
# source: llm
# inputs: none
# description: transpose a 3x3 matrix into a second buffer
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
m: .word 1, 2, 3, 4, 5, 6, 7, 8, 9
t: .word 0, 0, 0, 0, 0, 0, 0, 0, 0
.text
la a0, m
la a1, t
li t0, 0
li t6, 3
rows:
li t1, 0
cols:
mul t2, t0, t6
add t2, t2, t1
slli t2, t2, 2
add t2, t2, a0
lw t3, 0(t2)
mul t4, t1, t6
add t4, t4, t0
slli t4, t4, 2
add t4, t4, a1
sw t3, 0(t4)
addi t1, t1, 1
blt t1, t6, cols
addi t0, t0, 1
blt t0, t6, rows
li a7, 10
ecall
