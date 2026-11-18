# name: mixed_checksum
# category: mixed
# source: llm
# inputs: none
# description: additive and xor checksums over 6 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
data: .word 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
out: .word 0, 0
.text
la a0, data
lw t0, 0(a0)
lw t1, 4(a0)
lw t2, 8(a0)
lw t3, 12(a0)
lw t4, 16(a0)
lw t5, 20(a0)
add t6, t0, t1
xor s0, t0, t1
add t6, t6, t2
xor s0, s0, t2
add t6, t6, t3
xor s0, s0, t3
add t6, t6, t4
xor s0, s0, t4
add t6, t6, t5
xor s0, s0, t5
andi t6, t6, 255
sw t6, 24(a0)
sw s0, 28(a0)
li a7, 10
ecall
