# name: mixed_min_max_mean
# category: mixed
# source: llm
# inputs: none
# description: min, max and mean of 8 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 14, 3, 27, 8, 19, 5, 31, 12
out: .word 0, 0, 0
.text
la a0, arr
li t5, 8
lw t1, 0(a0)
mv t2, t1
li t3, 0
mv t4, a0
loop:
lw t0, 0(t4)
add t3, t3, t0
bge t0, t1, notmin
mv t1, t0
notmin:
ble t0, t2, notmax
mv t2, t0
notmax:
addi t4, t4, 4
addi t5, t5, -1
bnez t5, loop
li t6, 8
div t3, t3, t6
sw t1, 32(a0)
sw t2, 36(a0)
sw t3, 40(a0)
li a7, 10
ecall
