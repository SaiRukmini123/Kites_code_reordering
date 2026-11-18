# Array sum: store 4 values, then loop-sum them.
addi x20, x0, 1000
addi x21, x0, 4
addi x22, x0, 0
addi x23, x0, 0
sw x21, 0(x20)
sw x21, 4(x20)
sw x21, 8(x20)
sw x21, 12(x20)
LOOP:
beq x23, x21, END
lw x24, 0(x20)
add x22, x22, x24
addi x20, x20, 4
addi x23, x23, 1
jal x0, LOOP
END:
