# name: hazard_mul_use
# category: hazard
# source: llm
# inputs: none
# description: loads feeding mul feeding add
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
v: .word 6, 7, 8, 9
out: .word 0, 0
.text
la a0, v
lw t0, 0(a0)
lw t1, 4(a0)
mul t2, t0, t1
add t3, t2, t0
lw t4, 8(a0)
lw t5, 12(a0)
mul t6, t4, t5
sub s0, t6, t4
sw t3, 16(a0)
sw s0, 20(a0)
li a7, 10
ecall
