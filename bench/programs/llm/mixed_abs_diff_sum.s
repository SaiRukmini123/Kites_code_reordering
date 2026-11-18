# name: mixed_abs_diff_sum
# category: mixed
# source: llm
# inputs: none
# description: sum of |a[i] - b[i]| with a branch-free abs
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
a: .word 5, 17, 2, 9, 30, 4
b: .word 8, 10, 2, 15, 1, 4
out: .word 0
.text
la a0, a
li t5, 6
li t3, 0
loop:
lw t0, 0(a0)
lw t1, 24(a0)
sub t2, t0, t1
srai t4, t2, 63
xor t2, t2, t4
sub t2, t2, t4
add t3, t3, t2
addi a0, a0, 4
addi t5, t5, -1
bnez t5, loop
la a1, out
sw t3, 0(a1)
li a7, 10
ecall
