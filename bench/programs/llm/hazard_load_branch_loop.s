# name: hazard_load_branch_loop
# category: hazard
# source: llm
# inputs: none
# description: loop that branches on a value it has just loaded
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
arr: .word 4, 8, 15, 16, 23, 42, 0
out: .word 0
.text
la a0, arr
li t2, 0
loop:
lw t0, 0(a0)
beq t0, x0, done
add t2, t2, t0
addi a0, a0, 4
j loop
done:
la a1, out
sw t2, 0(a1)
li a7, 10
ecall
