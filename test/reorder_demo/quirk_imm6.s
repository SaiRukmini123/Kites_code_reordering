# Quirk program, imm = 6: same as quirk_imm5.s, but the immediate bits are 6,
# which matches nothing in flight, so there is no (false) stall.
addi x10, x0, 64
lw   x5, 0(x10)
addi x6, x0, 6
