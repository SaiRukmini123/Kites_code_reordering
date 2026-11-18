# load -> addi -> store chain, then two ALU chains that join
# expected stalls: rv5-h-f=1 rv5-h-nf=10
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
lw x5, 0(x10)
addi x6, x5, 32
sw x6, 4(x10)
addi x11, x0, 64
add x12, x11, x11
addi x13, x0, 32
add x14, x13, x13
add x15, x12, x14
