# name: chain_fibonacci
# category: chain
# source: llm
# inputs: none
# description: iterative Fibonacci (each step needs the previous)
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
n: .word 20
out: .word 0
.text
la a0, n
lw t0, 0(a0)
li t1, 0
li t2, 1
loop:
add t3, t1, t2
mv t1, t2
mv t2, t3
addi t0, t0, -1
bnez t0, loop
sw t1, 4(a0)
li a7, 10
ecall
