# name: mixed_saturating_add
# category: mixed
# source: llm
# inputs: none
# description: element-wise add clamped to 255
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
a: .word 100, 250, 30, 200, 255, 0
b: .word 100, 10, 240, 90, 1, 5
c: .word 0, 0, 0, 0, 0, 0
.text
la a0, a
la a1, b
la a2, c
li t5, 6
li t6, 255
loop:
lw t0, 0(a0)
lw t1, 0(a1)
add t2, t0, t1
ble t2, t6, ok
mv t2, t6
ok:
sw t2, 0(a2)
addi a0, a0, 4
addi a1, a1, 4
addi a2, a2, 4
addi t5, t5, -1
bnez t5, loop
li a7, 10
ecall
