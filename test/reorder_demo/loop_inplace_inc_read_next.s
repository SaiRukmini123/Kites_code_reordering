# In-place increment a[i] += 1 that also sums the next element a[i+1]:
# a load of a different element follows a store off the same base pointer.
addi x20, x0, 1500
addi x21, x0, 4
addi x22, x0, 0
addi x23, x0, 0
addi x5, x0, 10
sw x5, 0(x20)
addi x5, x0, 20
sw x5, 4(x20)
addi x5, x0, 30
sw x5, 8(x20)
addi x5, x0, 40
sw x5, 12(x20)
addi x5, x0, 50
sw x5, 16(x20)
LOOPA:
beq x22, x21, ENDA
lw x24, 0(x20)
addi x24, x24, 1
sw x24, 0(x20)
lw x25, 4(x20)
add x23, x23, x25
addi x20, x20, 4
addi x22, x22, 1
jal x0, LOOPA
ENDA:
