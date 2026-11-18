# two load-use pairs plus two independent addi at the end
# expected stalls: rv5-h-f=2 rv5-h-nf=4
# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds
# x10 = 0x10000000 and mem[0x10000000] = 7 (so the programs need no la/li).
lw x5, 0(x10)
add x6, x5, x5
lw x7, 4(x10)
add x8, x7, x7
addi x11, x0, 32
addi x12, x0, 64
