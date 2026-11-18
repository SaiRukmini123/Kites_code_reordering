# name: loop_vector_add
# category: loop
# source: llm
# inputs: none
# description: c[i] = a[i] + b[i] for 8 elements
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
a: .word 10, 20, 30, 40, 50, 60, 70, 80
b: .word 1, 2, 3, 4, 5, 6, 7, 8
c: .word 0, 0, 0, 0, 0, 0, 0, 0
.text
la a0, a
la a1, b
la a2, c
li t3, 8
loop:
lw t0, 0(a0)
lw t1, 0(a1)
add t2, t0, t1
sw t2, 0(a2)
addi a0, a0, 4
addi a1, a1, 4
addi a2, a2, 4
addi t3, t3, -1
bnez t3, loop
li a7, 10
ecall
