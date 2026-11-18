# name: aliasing_store_then_reload
# category: aliasing
# source: llm
# inputs: none
# description: store then reload the same word; a second pointer aliases the next word
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
x: .word 5
y: .word 0
.text
la a0, x
li t0, 9
sw t0, 0(a0)
lw t1, 0(a0)
add t2, t1, t1
la a1, y
lw t3, 0(a1)
addi t3, t3, 1
sw t3, 0(a1)
sw t2, 4(a0)
li a7, 10
ecall
