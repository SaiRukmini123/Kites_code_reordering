# name: loop_strlen
# category: loop
# source: llm
# inputs: none
# description: length of a zero-terminated byte string
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
len: .word 0
str: .byte 72, 101, 108, 108, 111, 44, 32, 75, 105, 116, 101, 115, 0
.text
la a0, str
li t1, 0
loop:
add t2, a0, t1
lbu t0, 0(t2)
beq t0, x0, done
addi t1, t1, 1
j loop
done:
la a1, len
sw t1, 0(a1)
li a7, 10
ecall
