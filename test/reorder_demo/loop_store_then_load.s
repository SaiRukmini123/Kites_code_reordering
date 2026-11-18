# Store then load at the SAME address each iteration: the dependency graph
# must keep them ordered.
addi x20, x0, 1200
addi x21, x0, 3
addi x22, x0, 0
addi x23, x0, 0
LOOP3:
beq x22, x21, END3
sw x22, 0(x20)
lw x24, 0(x20)
add x23, x23, x24
addi x20, x20, 4
addi x22, x22, 1
jal x0, LOOP3
END3:
