# name: aliasing_histogram
# category: aliasing
# source: llm
# inputs: none
# description: histogram: counts[v]++ through computed addresses
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
vals: .word 2, 0, 3, 2, 1, 2, 3, 0, 2, 1
hist: .word 0, 0, 0, 0
.text
la a0, vals
la a1, hist
li t3, 10
loop:
lw t0, 0(a0)
slli t0, t0, 2
add t1, a1, t0
lw t2, 0(t1)
addi t2, t2, 1
sw t2, 0(t1)
addi a0, a0, 4
addi t3, t3, -1
bnez t3, loop
li a7, 10
ecall
