# Load-use inside the loop body: the lw result is consumed immediately.
addi x20, x0, 1100
addi x21, x0, 3
addi x22, x0, 0
sw x21, 0(x20)
sw x21, 4(x20)
sw x21, 8(x20)
LOOP2:
beq x22, x21, END2
lw x24, 0(x20)
add x25, x24, x24
addi x20, x20, 4
addi x22, x22, 1
jal x0, LOOP2
END2:
