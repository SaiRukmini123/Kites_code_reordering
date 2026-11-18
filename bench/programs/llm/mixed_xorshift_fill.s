# name: mixed_xorshift_fill
# category: mixed
# source: llm
# inputs: none
# description: fill 8 words with a 32-bit xorshift generator
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
seed: .word 123456789
out: .word 0, 0, 0, 0, 0, 0, 0, 0
.text
la a0, seed
lw t0, 0(a0)
addi a1, a0, 4
li t3, 8
li t4, -1
srli t4, t4, 32
and t0, t0, t4
loop:
slli t1, t0, 13
xor t0, t0, t1
and t0, t0, t4
srli t1, t0, 17
xor t0, t0, t1
slli t1, t0, 5
xor t0, t0, t1
and t0, t0, t4
sw t0, 0(a1)
addi a1, a1, 4
addi t3, t3, -1
bnez t3, loop
li a7, 10
ecall
