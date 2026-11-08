#include <gtest/gtest.h>

#include <sstream>
#include <vector>

#include "assembler/assembler.h"
#include "reorder/block_finder.h"

using namespace Kites::reorder;

namespace
{

// Small hand encoders so the tests don't depend on the assembler.
uint32_t addi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t add(int rd, int rs1, int rs2)  { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33; }

uint32_t beq(int rs1, int rs2, int off)
{
    uint32_t i = static_cast<uint32_t>(off);
    return (((i >> 12) & 1) << 31) | (((i >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) |
           (((i >> 1) & 0xF) << 8) | (((i >> 11) & 1) << 7) | 0x63;
}

uint32_t jal(int rd, int off)
{
    uint32_t i = static_cast<uint32_t>(off);
    return (((i >> 20) & 1) << 31) | (((i >> 1) & 0x3FF) << 21) | (((i >> 11) & 1) << 20) |
           (((i >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}

uint32_t jalr(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x67; }
uint32_t auipc(int rd, int imm20)       { return ((imm20 & 0xFFFFF) << 12) | (rd << 7) | 0x17; }

const uint32_t kEcall = 0x00000073;

void expectBlock(const Block &b, size_t begin, size_t end, bool term)
{
    EXPECT_EQ(b.begin, begin);
    EXPECT_EQ(b.end, end);
    EXPECT_EQ(b.hasTerminator, term);
}

} // namespace

TEST(ReorderBlockFinder, ImmediateHelpers)
{
    EXPECT_EQ(branchImm(beq(1, 0, 8)), 8);
    EXPECT_EQ(branchImm(beq(1, 2, -4)), -4);
    EXPECT_EQ(jalImm(jal(1, 8)), 8);
    EXPECT_EQ(jalImm(jal(0, -4)), -4);
}

TEST(ReorderBlockFinder, WorkedExample)
{
    // 0 addi; 1 beq -> index 4; 2 addi; 3 addi; 4 add; 5 ecall
    auto b = findBlocks({addi(1, 0, 5), beq(1, 0, 12), addi(2, 0, 1), addi(3, 0, 2),
                         add(4, 2, 3), kEcall});
    ASSERT_EQ(b.size(), 4u);
    expectBlock(b[0], 0, 2, true);
    expectBlock(b[1], 2, 4, false);
    expectBlock(b[2], 4, 5, false);
    expectBlock(b[3], 5, 6, false);
}

TEST(ReorderBlockFinder, StraightLine)
{
    auto b = findBlocks({addi(1, 0, 1), addi(2, 0, 2), add(3, 1, 2)});
    ASSERT_EQ(b.size(), 1u);
    expectBlock(b[0], 0, 3, false);
}

TEST(ReorderBlockFinder, BackwardLoop)
{
    // 0 addi; 1 addi (loop top); 2 add; 3 beq -> index 1; 4 addi
    auto b = findBlocks({addi(1, 0, 3), addi(1, 1, -1), add(2, 2, 1), beq(1, 0, -8), addi(5, 0, 9)});
    ASSERT_EQ(b.size(), 3u);
    expectBlock(b[0], 0, 1, false);
    expectBlock(b[1], 1, 4, true);
    expectBlock(b[2], 4, 5, false);
}

TEST(ReorderBlockFinder, JalCall)
{
    // 0 addi; 1 jal -> index 3; 2 addi (skipped); 3 add
    auto b = findBlocks({addi(1, 0, 1), jal(1, 8), addi(2, 0, 2), add(3, 1, 1)});
    ASSERT_EQ(b.size(), 3u);
    expectBlock(b[0], 0, 2, true);
    expectBlock(b[1], 2, 3, false);
    expectBlock(b[2], 3, 4, false);
}

TEST(ReorderBlockFinder, Empty)
{
    EXPECT_TRUE(findBlocks({}).empty());
}

// jalr has no decodable target, but it must still end its block like any other jump.
TEST(ReorderBlockFinder, JalrIsTerminatorAndBarrier)
{
    // 0 jal -> index 2; 1 addi (skipped on the jump); 2 addi; 3 jalr (return); 4 addi
    auto b = findBlocks({jal(1, 12), addi(5, 0, 1), addi(6, 0, 2), jalr(0, 1, 0), addi(7, 0, 3)});
    ASSERT_EQ(b.size(), 4u);
    expectBlock(b[0], 0, 1, true);   // jal alone
    expectBlock(b[1], 1, 3, false);  // the skipped addi plus the call target
    expectBlock(b[2], 3, 4, true);   // jalr alone, also a terminator
    expectBlock(b[3], 4, 5, false);
}

// auipc is pinned to its address, so nothing may move across it in either direction.
TEST(ReorderBlockFinder, AuipcIsIsolated)
{
    auto b = findBlocks({addi(1, 0, 1), auipc(2, 0), addi(3, 0, 2)});
    ASSERT_EQ(b.size(), 3u);
    expectBlock(b[0], 0, 1, false);
    expectBlock(b[1], 1, 2, false);  // auipc is a barrier, not a branch/jump, so hasTerminator is false
    expectBlock(b[2], 2, 3, false);
}

// A branch target outside the program must not crash and must not fabricate a leader.
TEST(ReorderBlockFinder, OutOfRangeTargetIsIgnored)
{
    auto b = findBlocks({beq(0, 0, 400), addi(1, 0, 1)});
    ASSERT_EQ(b.size(), 2u);
    expectBlock(b[0], 0, 1, true);
    expectBlock(b[1], 1, 2, false);
}

// extraLeaders lets a caller force a split that findBlocks can't see on its own.
TEST(ReorderBlockFinder, ExtraLeaderSplitsBlock)
{
    auto b = findBlocks({addi(1, 0, 1), addi(2, 0, 2), addi(3, 0, 3), addi(4, 0, 4)}, {2});
    ASSERT_EQ(b.size(), 2u);
    expectBlock(b[0], 0, 2, false);
    expectBlock(b[1], 2, 4, false);
}

// The real gap this closes: a label that no branch or jal in the text points to
// (the way a jalr target wouldn't be) is invisible to plain findBlocks(), but
// labelLeaders() recovers it from the assembler's symbol table.
TEST(ReorderBlockFinder, LabelLeadersCatchesJalrOnlyTargets)
{
    std::istringstream src(
        "addi x1, x0, 1\n"
        "addi x2, x0, 2\n"
        "L:\n"
        "addi x3, x0, 3\n"
        "ecall\n");
    Kites::AssembledProgram program = Kites::assemble(src);

    auto plain = findBlocks(program.text_buffer);
    ASSERT_EQ(plain.size(), 2u);               // L is invisible: nothing in the text targets it
    expectBlock(plain[0], 0, 3, false);
    expectBlock(plain[1], 3, 4, false);

    auto withLabels = findBlocks(program.text_buffer, labelLeaders(program));
    ASSERT_EQ(withLabels.size(), 3u);          // L now starts its own block
    expectBlock(withLabels[0], 0, 2, false);
    expectBlock(withLabels[1], 2, 3, false);
    expectBlock(withLabels[2], 3, 4, false);
}

// End-to-end: real assembler output, so we know our encodings match KITES's.
TEST(ReorderBlockFinder, AssemblerOutput)
{
    std::istringstream src(
        "addi x1, x0, 5\n"
        "beq  x1, x0, L\n"
        "addi x2, x0, 1\n"
        "addi x3, x0, 2\n"
        "L:\n"
        "add  x4, x2, x3\n"
        "ecall\n");
    Kites::AssembledProgram program = Kites::assemble(src);
    auto b = findBlocks(program.text_buffer);
    ASSERT_EQ(b.size(), 4u);
    expectBlock(b[0], 0, 2, true);
    expectBlock(b[1], 2, 4, false);
    expectBlock(b[2], 4, 5, false);
    expectBlock(b[3], 5, 6, false);
}
