# name: hazard_shift_mask
# category: hazard
# source: llm
# inputs: none
# description: bit-field extraction with shifts and masks
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
w: .word 0x12345678
out: .word 0, 0
.text
la a0, w
lw t0, 0(a0)
srli t1, t0, 16
andi t2, t0, 255
slli t3, t1, 4
or t4, t3, t2
li t5, 0xff00
and t6, t0, t5
xor s0, t4, t6
sw s0, 4(a0)
sw t6, 8(a0)
li a7, 10
ecall
