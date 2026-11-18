# name: hazard_byte_loads
# category: hazard
# source: llm
# inputs: none
# description: byte loads (signed and unsigned) used immediately
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
b: .byte 1, 2, 3, 4, 250, 6, 7, 8
out: .word 0
.text
la a0, b
lbu t0, 0(a0)
add t1, t0, t0
lbu t2, 1(a0)
add t3, t2, t1
lb t4, 4(a0)
add t5, t4, t3
lbu t6, 7(a0)
add s0, t6, t5
sw s0, 8(a0)
li a7, 10
ecall
