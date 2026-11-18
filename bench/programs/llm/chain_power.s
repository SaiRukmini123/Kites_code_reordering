# name: chain_power
# category: chain
# source: llm
# inputs: none
# description: 3^13 by square-and-multiply
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
base: .word 3
exp: .word 13
out: .word 0
.text
la a0, base
lw t0, 0(a0)
lw t1, 4(a0)
li t2, 1
loop:
beqz t1, done
andi t3, t1, 1
beqz t3, skip
mul t2, t2, t0
skip:
mul t0, t0, t0
srli t1, t1, 1
j loop
done:
sw t2, 8(a0)
li a7, 10
ecall
