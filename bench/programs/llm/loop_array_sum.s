# name: loop_array_sum
# category: loop
# source: llm
# inputs: none
# description: sum of 16 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3
sum: .word 0
.text
la a0, arr
li t1, 16
li t2, 0
loop:
lw t0, 0(a0)
add t2, t2, t0
addi a0, a0, 4
addi t1, t1, -1
bne t1, x0, loop
la a1, sum
sw t2, 0(a1)
li a7, 10
ecall
