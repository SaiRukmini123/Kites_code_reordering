# branch right after its load (not taken, word0=7)
# expected stalls: rv5-h-f=1 rv5-h-nf=2
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
lw x5, 0(x10)
beq x5, x0, done
addi x6, x0, 32
done:
addi x7, x0, 64
