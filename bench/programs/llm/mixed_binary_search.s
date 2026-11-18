# name: mixed_binary_search
# category: mixed
# source: llm
# inputs: none
# description: binary search for 23 in a sorted array of 10
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 2, 5, 8, 12, 16, 23, 38, 56, 72, 91
key: .word 23
idx: .word -1
.text
la a0, arr
lw t6, 40(a0)
li t0, 0
li t1, 9
search:
bgt t0, t1, done
add t2, t0, t1
srli t2, t2, 1
slli t3, t2, 2
add t3, t3, a0
lw t4, 0(t3)
beq t4, t6, found
blt t4, t6, golow
addi t1, t2, -1
j search
golow:
addi t0, t2, 1
j search
found:
sw t2, 44(a0)
done:
li a7, 10
ecall
