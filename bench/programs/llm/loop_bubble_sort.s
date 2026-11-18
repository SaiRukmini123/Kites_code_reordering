# name: loop_bubble_sort
# category: loop
# source: llm
# inputs: none
# description: bubble sort of 8 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 29, 3, 17, 8, 42, 1, 23, 11
.text
la a0, arr
li t5, 7
outer:
mv t0, a0
mv t6, t5
inner:
lw t1, 0(t0)
lw t2, 4(t0)
ble t1, t2, noswap
sw t2, 0(t0)
sw t1, 4(t0)
noswap:
addi t0, t0, 4
addi t6, t6, -1
bnez t6, inner
addi t5, t5, -1
bnez t5, outer
li a7, 10
ecall
