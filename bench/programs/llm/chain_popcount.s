# name: chain_popcount
# category: chain
# source: llm
# inputs: none
# description: population count by clearing the lowest set bit
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
v: .word 0x5a5a1234
out: .word 0
.text
la a0, v
lw t0, 0(a0)
li t1, 0
loop:
beqz t0, done
addi t2, t0, -1
and t0, t0, t2
addi t1, t1, 1
j loop
done:
sw t1, 4(a0)
li a7, 10
ecall
