# name: hazard_alu_pairs
# category: hazard
# source: llm
# inputs: none
# description: short ALU producer/consumer pairs with independent constants
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
out: .word 0, 0, 0, 0
.text
la a0, out
li t0, 7
addi t1, t0, 3
li t2, 11
add t3, t2, t2
li t4, 13
sub t5, t4, t0
add t6, t1, t3
add t6, t6, t5
sw t6, 0(a0)
li a7, 10
ecall
