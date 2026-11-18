# name: chain_factorial
# category: chain
# source: llm
# inputs: none
# description: 10! by repeated multiply
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
n: .word 10
out: .word 0
.text
la a0, n
lw t0, 0(a0)
li t1, 1
loop:
mul t1, t1, t0
addi t0, t0, -1
bnez t0, loop
sw t1, 4(a0)
li a7, 10
ecall
