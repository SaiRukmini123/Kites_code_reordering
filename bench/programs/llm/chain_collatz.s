# name: chain_collatz
# category: chain
# source: llm
# inputs: none
# description: Collatz steps for 27
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
n: .word 27
steps: .word 0
.text
la a0, n
lw t0, 0(a0)
li t1, 0
li t3, 1
loop:
beq t0, t3, done
andi t2, t0, 1
beqz t2, even
slli t4, t0, 1
add t0, t0, t4
addi t0, t0, 1
j next
even:
srli t0, t0, 1
next:
addi t1, t1, 1
j loop
done:
sw t1, 4(a0)
li a7, 10
ecall
