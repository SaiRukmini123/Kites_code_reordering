# ALU result used 2 later
# expected stalls: rv5-h-f=0 rv5-h-nf=1
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
addi x5, x0, 64
addi x7, x0, 32
add x6, x5, x5
