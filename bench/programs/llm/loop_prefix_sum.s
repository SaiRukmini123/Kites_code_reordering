# name: loop_prefix_sum
# category: loop
# source: llm
# inputs: none
# description: in-place prefix sum of 10 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 2, 4, 6, 8, 10, 12, 14, 16, 18, 20
.text
la a0, arr
li t1, 9
lw t2, 0(a0)
loop:
lw t0, 4(a0)
add t2, t2, t0
sw t2, 4(a0)
addi a0, a0, 4
addi t1, t1, -1
bnez t1, loop
li a7, 10
ecall
