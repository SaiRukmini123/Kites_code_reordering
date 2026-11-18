# name: loop_count_evens
# category: loop
# source: llm
# inputs: none
# description: count even numbers in 12 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 5, 8, 13, 22, 7, 40, 11, 6, 9, 14, 3, 2
count: .word 0
.text
la a0, arr
li t1, 12
li t3, 0
loop:
lw t0, 0(a0)
andi t2, t0, 1
bnez t2, odd
addi t3, t3, 1
odd:
addi a0, a0, 4
addi t1, t1, -1
bnez t1, loop
la a1, count
sw t3, 0(a1)
li a7, 10
ecall
