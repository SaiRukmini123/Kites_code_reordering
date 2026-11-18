# name: chain_hash_mix
# category: chain
# source: llm
# inputs: none
# description: djb2-style hash over 4 words (shift-add-xor chain)
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
words: .word 0x1234, 0x9abc, 0x0f0f, 0x7777
h: .word 0
.text
la a0, words
li t0, 5381
lw t1, 0(a0)
slli t2, t0, 5
add t0, t0, t2
xor t0, t0, t1
lw t1, 4(a0)
slli t2, t0, 5
add t0, t0, t2
xor t0, t0, t1
lw t1, 8(a0)
slli t2, t0, 5
add t0, t0, t2
xor t0, t0, t1
lw t1, 12(a0)
slli t2, t0, 5
add t0, t0, t2
xor t0, t0, t1
sw t0, 16(a0)
li a7, 10
ecall
