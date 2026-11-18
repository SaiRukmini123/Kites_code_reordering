# Quirk program, imm = 5: the HDU reads bits 20-24 of addi as rs2. Here those
# immediate bits are 5, which matches the load's rd x5, so the VM stalls even
# though addi does not read x5.
addi x10, x0, 64
lw   x5, 0(x10)
addi x6, x0, 5
