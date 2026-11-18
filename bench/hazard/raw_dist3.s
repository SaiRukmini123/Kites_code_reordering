# ALU result used 3 later: no hazard
# expected stalls: rv5-h-f=0 rv5-h-nf=0
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
addi x5, x0, 64
addi x7, x0, 32
addi x8, x0, 32
add x6, x5, x5
