# name: loop_linked_list
# category: loop
# source: llm
# inputs: none
# description: linked-list traversal (next field is a node index, -1 ends)
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
nodes: .word 10, 3, 40, -1, 20, 4, 30, 1, 50, 2
sum: .word 0
.text
la a0, nodes
li t0, 0
li t3, 0
li t4, -1
walk:
slli t1, t0, 3
add t1, t1, a0
lw t2, 0(t1)
add t3, t3, t2
lw t0, 4(t1)
bne t0, t4, walk
la a1, sum
sw t3, 0(a1)
li a7, 10
ecall
