# name: hazard_load_use_sum3
# category: hazard
# source: llm
# inputs: none
# description: three loads, each used right away
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
a: .word 5, 9, 14
out: .word 0
.text
la t0, a
lw t1, 0(t0)
addi t1, t1, 1
lw t2, 4(t0)
addi t2, t2, 2
lw t3, 8(t0)
add t4, t1, t2
add t4, t4, t3
sw t4, 12(t0)
li a7, 10
ecall
