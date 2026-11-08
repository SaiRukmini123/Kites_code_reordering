#include <gtest/gtest.h>
#include "reorder/decoded_inst.h"

using namespace Kites::reorder;

TEST(ReorderDecoder, Load)
{
    auto d = decode(0x00052283);
    EXPECT_TRUE(d.is_load);
    EXPECT_EQ(d.rd, 5);
    EXPECT_EQ(d.rs1, 10);
    EXPECT_TRUE(d.reads_rs1);
    EXPECT_FALSE(d.reads_rs2);
}

TEST(ReorderDecoder, AddiDoesNotReadRs2)
{
    auto d = decode(0x00500313);
    EXPECT_EQ(d.rs2, 5);          // raw bits look like x5
    EXPECT_FALSE(d.reads_rs2);    // but it is really the immediate
}

TEST(ReorderDecoder, Store)
{
    auto d = decode(0x0020A023);          // sw x2,0(x1)
    EXPECT_TRUE(d.is_store);
    EXPECT_EQ(d.rs1, 1);
    EXPECT_EQ(d.rs2, 2);
    EXPECT_TRUE(d.reads_rs2);
    EXPECT_FALSE(d.writes_rd);
}

TEST(ReorderDecoder, Ecall)
{
    auto d = decode(0x00000073);
    EXPECT_EQ(d.kind, Kind::Ecall);
    EXPECT_TRUE(d.is_barrier);
}

TEST(ReorderDecoder, WForms)
{
    auto a = decode(0x0052833B);          // addw x6,x5,x5
    EXPECT_EQ(a.kind, Kind::Op);
    EXPECT_TRUE(a.reads_rs2);
    EXPECT_FALSE(a.is_barrier);

    auto b = decode(0x0050031B);          // addiw x6,x0,5
    EXPECT_EQ(b.kind, Kind::OpImm);
    EXPECT_FALSE(b.reads_rs2);
    EXPECT_EQ(b.rs1, 0);
}

TEST(ReorderDecoder, Branch)
{
    auto d = decode(0x00208063);          // beq x1,x2,0
    EXPECT_EQ(d.kind, Kind::Branch);
    EXPECT_TRUE(d.is_branch);
    EXPECT_TRUE(d.is_barrier);
    EXPECT_TRUE(d.reads_rs1);
    EXPECT_TRUE(d.reads_rs2);
    EXPECT_FALSE(d.writes_rd);
}

TEST(ReorderDecoder, Jal)
{
    auto d = decode(0x000000EF);          // jal x1,0
    EXPECT_EQ(d.kind, Kind::Jal);
    EXPECT_TRUE(d.is_jump);
    EXPECT_TRUE(d.is_barrier);
    EXPECT_TRUE(d.writes_rd);
    EXPECT_EQ(d.rd, 1);
    EXPECT_FALSE(d.reads_rs1);             // jal has no source register at all
}

TEST(ReorderDecoder, Jalr)
{
    auto d = decode(0x00008067);          // jalr x0,0(x1)
    EXPECT_EQ(d.kind, Kind::Jalr);
    EXPECT_TRUE(d.is_jump);
    EXPECT_TRUE(d.is_barrier);
    EXPECT_TRUE(d.reads_rs1);
    EXPECT_EQ(d.rs1, 1);
    EXPECT_TRUE(d.writes_rd);              // still true even when rd is x0
    EXPECT_EQ(d.rd, 0);
}

TEST(ReorderDecoder, Lui)
{
    auto d = decode(0x000012B7);          // lui x5,1
    EXPECT_EQ(d.kind, Kind::Lui);
    EXPECT_TRUE(d.writes_rd);
    EXPECT_EQ(d.rd, 5);
    EXPECT_FALSE(d.reads_rs1);
    EXPECT_FALSE(d.reads_rs2);
    EXPECT_FALSE(d.is_barrier);            // unlike auipc, lui doesn't depend on its own address
}

TEST(ReorderDecoder, Auipc)
{
    auto d = decode(0x00000097);          // auipc x1,0
    EXPECT_EQ(d.kind, Kind::Auipc);
    EXPECT_TRUE(d.writes_rd);
    EXPECT_TRUE(d.is_barrier);
    EXPECT_FALSE(d.is_jump);
}

TEST(ReorderDecoder, MulUsesOpEncoding)
{
    // mul x3,x1,x2 -- M-extension instructions share the OP opcode with funct7=1,
    // so they need no special case here; this just confirms that's really true.
    auto d = decode(0x022081B3);
    EXPECT_EQ(d.kind, Kind::Op);
    EXPECT_TRUE(d.writes_rd);
    EXPECT_TRUE(d.reads_rs1);
    EXPECT_TRUE(d.reads_rs2);
    EXPECT_FALSE(d.is_barrier);
    EXPECT_EQ(d.rd, 3);
    EXPECT_EQ(d.rs1, 1);
    EXPECT_EQ(d.rs2, 2);
}

TEST(ReorderDecoder, UnknownOpcodeIsBarrier)
{
    auto d = decode(0x0000007F);          // opcode 0x7F doesn't exist in RV64IM
    EXPECT_EQ(d.kind, Kind::Other);
    EXPECT_TRUE(d.is_barrier);
    EXPECT_FALSE(d.writes_rd);
}