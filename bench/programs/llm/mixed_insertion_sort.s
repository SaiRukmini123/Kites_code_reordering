# name: mixed_insertion_sort
# category: mixed
# source: llm
# inputs: none
# description: insertion sort of 8 words
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 9, 4, 7, 1, 8, 2, 6, 3
.text
la a0, arr
li t0, 1
li t6, 8
outer:
slli t1, t0, 2
add t1, t1, a0
lw t2, 0(t1)
addi t3, t0, -1
inner:
blt t3, x0, place
slli t4, t3, 2
add t4, t4, a0
lw t5, 0(t4)
ble t5, t2, place
sw t5, 4(t4)
addi t3, t3, -1
j inner
place:
addi t4, t3, 1
slli t4, t4, 2
add t4, t4, a0
sw t2, 0(t4)
addi t0, t0, 1
blt t0, t6, outer
li a7, 10
ecall
