# two independent 3-long ALU chains, written one after the other
# expected stalls: rv5-h-f=0 rv5-h-nf=8
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
addi x5, x0, 64
add x6, x5, x5
add x7, x6, x6
addi x11, x0, 32
add x12, x11, x11
add x13, x12, x12
