# name: hazard_load_use_pairs_wide
# category: hazard
# source: llm
# inputs: none
# description: six independent load/double pairs, then sums
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
in: .word 1, 2, 3, 4, 5, 6
out: .word 0, 0, 0
.text
la a0, in
lw t0, 0(a0)
add t0, t0, t0
lw t1, 4(a0)
add t1, t1, t1
lw t2, 8(a0)
add t2, t2, t2
lw t3, 12(a0)
add t3, t3, t3
lw t4, 16(a0)
add t4, t4, t4
lw t5, 20(a0)
add t5, t5, t5
add s0, t0, t1
add s1, t2, t3
add s2, t4, t5
sw s0, 24(a0)
sw s1, 28(a0)
sw s2, 32(a0)
li a7, 10
ecall
