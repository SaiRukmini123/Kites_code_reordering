# Array copy b[i] = a[i] with b = a + 32 bytes off the same pointer, loading
# a[i+1] right after the store to b[i].
addi x20, x0, 1700
addi x21, x0, 4
addi x22, x0, 0
addi x23, x0, 0
addi x5, x0, 7
sw x5, 0(x20)
addi x5, x0, 8
sw x5, 4(x20)
addi x5, x0, 9
sw x5, 8(x20)
addi x5, x0, 10
sw x5, 12(x20)
addi x5, x0, 11
sw x5, 16(x20)
LOOPB:
beq x22, x21, ENDB
lw x24, 0(x20)
sw x24, 32(x20)
lw x25, 4(x20)
add x23, x23, x25
addi x20, x20, 4
addi x22, x22, 1
jal x0, LOOPB
ENDB:
