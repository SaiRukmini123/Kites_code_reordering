# name: aliasing_byte_into_word
# category: aliasing
# source: llm
# inputs: none
# description: byte stores that overlap word loads
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
w: .word 0x11223344
out: .word 0
.text
la a0, w
li t0, 0x7f
sb t0, 1(a0)
lw t1, 0(a0)
addi t1, t1, 1
sw t1, 4(a0)
lbu t2, 0(a0)
lbu t3, 3(a0)
add t4, t2, t3
sb t4, 2(a0)
li a7, 10
ecall
