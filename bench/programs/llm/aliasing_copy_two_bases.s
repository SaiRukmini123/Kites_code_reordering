# name: aliasing_copy_two_bases
# category: aliasing
# source: llm
# inputs: none
# description: copy 4 words; source and destination use different base registers
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
src: .word 3, 1, 4, 1
dst: .word 0, 0, 0, 0
.text
la a0, src
la a1, dst
lw t0, 0(a0)
sw t0, 0(a1)
lw t1, 4(a0)
sw t1, 4(a1)
lw t2, 8(a0)
sw t2, 8(a1)
lw t3, 12(a0)
sw t3, 12(a1)
li a7, 10
ecall
