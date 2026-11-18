# name: loop_matmul_3x3
# category: loop
# source: llm
# inputs: none
# description: 3x3 integer matrix multiply with nested loops
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
A: .word 1, 2, 3, 4, 5, 6, 7, 8, 9
B: .word 9, 8, 7, 6, 5, 4, 3, 2, 1
C: .word 0, 0, 0, 0, 0, 0, 0, 0, 0
.text
la s0, A
la s1, B
la s2, C
li s3, 3
li t0, 0
iloop:
li t1, 0
jloop:
li t2, 0
li t6, 0
kloop:
mul t3, t0, s3
add t3, t3, t2
slli t3, t3, 2
add t3, t3, s0
lw t4, 0(t3)
mul t5, t2, s3
add t5, t5, t1
slli t5, t5, 2
add t5, t5, s1
lw t5, 0(t5)
mul t4, t4, t5
add t6, t6, t4
addi t2, t2, 1
blt t2, s3, kloop
mul t3, t0, s3
add t3, t3, t1
slli t3, t3, 2
add t3, t3, s2
sw t6, 0(t3)
addi t1, t1, 1
blt t1, s3, jloop
addi t0, t0, 1
blt t0, s3, iloop
li a7, 10
ecall
