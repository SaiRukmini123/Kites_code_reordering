# name: aliasing_swap_two_pointers
# category: aliasing
# source: llm
# inputs: none
# description: swap through one pointer, then update through an alias of it
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 11, 22, 33, 44, 55
.text
la a0, arr
addi a1, a0, 8
addi a2, a0, 8
lw t0, 0(a1)
lw t1, 16(a0)
sw t1, 0(a1)
sw t0, 16(a0)
lw t2, 0(a2)
addi t2, t2, 1
sw t2, 4(a2)
li a7, 10
ecall
