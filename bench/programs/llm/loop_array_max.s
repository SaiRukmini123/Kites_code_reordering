# name: loop_array_max
# category: loop
# source: llm
# inputs: none
# description: maximum of 12 signed words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 12, -5, 33, 7, 33, 0, 91, -40, 8, 15, 64, 2
max: .word 0
.text
la a0, arr
li t1, 11
lw t2, 0(a0)
addi a0, a0, 4
loop:
lw t0, 0(a0)
bge t2, t0, skip
mv t2, t0
skip:
addi a0, a0, 4
addi t1, t1, -1
bnez t1, loop
la a1, max
sw t2, 0(a1)
li a7, 10
ecall
