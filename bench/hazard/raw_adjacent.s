# ALU result used by next instr
# expected stalls: rv5-h-f=0 rv5-h-nf=2
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
addi x5, x0, 64
add x6, x5, x5
