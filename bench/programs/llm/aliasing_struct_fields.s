# name: aliasing_struct_fields
# category: aliasing
# source: llm
# inputs: none
# description: field-wise add of two structs, stores back into the first
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
p: .word 1, 2, 3, 4
q: .word 10, 20, 30, 40
.text
la a0, p
lw t0, 0(a0)
lw t1, 4(a0)
lw t2, 8(a0)
lw t3, 12(a0)
lw t4, 16(a0)
add t0, t0, t4
lw t5, 20(a0)
add t1, t1, t5
lw t6, 24(a0)
add t2, t2, t6
lw s0, 28(a0)
add t3, t3, s0
sw t0, 0(a0)
sw t1, 4(a0)
sw t2, 8(a0)
sw t3, 12(a0)
li a7, 10
ecall
