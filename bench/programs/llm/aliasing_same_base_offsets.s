# name: aliasing_same_base_offsets
# category: aliasing
# source: llm
# inputs: none
# description: read-modify-write of 4 words through one base register
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
buf: .word 1, 2, 3, 4, 5, 6, 7, 8
.text
la a0, buf
lw t0, 0(a0)
addi t0, t0, 10
sw t0, 0(a0)
lw t1, 4(a0)
addi t1, t1, 10
sw t1, 4(a0)
lw t2, 8(a0)
addi t2, t2, 10
sw t2, 8(a0)
lw t3, 12(a0)
addi t3, t3, 10
sw t3, 12(a0)
li a7, 10
ecall
