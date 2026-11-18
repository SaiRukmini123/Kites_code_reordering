# name: hazard_double_words
# category: hazard
# source: llm
# inputs: none
# description: 64-bit loads and stores with load-use pairs
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
d: .dword 100, 200, 300
out: .dword 0
.text
la a0, d
ld t0, 0(a0)
addi t0, t0, 1
ld t1, 8(a0)
addi t1, t1, 2
ld t2, 16(a0)
add t3, t0, t1
add t3, t3, t2
sd t3, 24(a0)
li a7, 10
ecall
