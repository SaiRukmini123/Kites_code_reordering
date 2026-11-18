# name: chain_gcd
# category: chain
# source: llm
# inputs: none
# description: Euclid's gcd with rem
# generated-by: Claude Code (Claude Opus 5.5), 2026-11-18; kept only if it passes the validity filter (kites-cli reorder-bench)
.data
ab: .word 1071, 462
g: .word 0
.text
la a0, ab
lw t0, 0(a0)
lw t1, 4(a0)
loop:
beqz t1, done
rem t2, t0, t1
mv t0, t1
mv t1, t2
j loop
done:
sw t0, 8(a0)
li a7, 10
ecall
