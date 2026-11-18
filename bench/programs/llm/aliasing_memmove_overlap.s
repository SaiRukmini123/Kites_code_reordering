# name: aliasing_memmove_overlap
# category: aliasing
# source: llm
# inputs: none
# description: overlapping copy (memmove backwards by one word)
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
buf: .word 1, 2, 3, 4, 5, 6, 7, 8
.text
la a0, buf
addi a1, a0, 4
li t3, 6
addi a0, a0, 20
addi a1, a1, 20
loop:
lw t0, 0(a0)
sw t0, 0(a1)
addi a0, a0, -4
addi a1, a1, -4
addi t3, t3, -1
bnez t3, loop
li a7, 10
ecall
