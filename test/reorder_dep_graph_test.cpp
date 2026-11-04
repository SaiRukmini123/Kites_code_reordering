#include <gtest/gtest.h>

#include <vector>

#include "reorder/dep_graph.h"

using namespace Kites::reorder;

namespace
{

// Hand encoders, same idea as in reorder_block_finder_test.cpp.
uint32_t addi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t add(int rd, int rs1, int rs2)  { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33; }
uint32_t lw(int rd, int rs1, int imm)   { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03; }
uint32_t sw(int rs2, int rs1, int imm)
{
    return (((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) | ((imm & 0x1F) << 7) | 0x23;
}
uint32_t beq(int rs1, int rs2, int off)
{
    uint32_t i = static_cast<uint32_t>(off);
    return (((i >> 12) & 1) << 31) | (((i >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) |
           (((i >> 1) & 0xF) << 8) | (((i >> 11) & 1) << 7) | 0x63;
}
const uint32_t kEcall = 0x00000073;

// Build the graph of the whole program treated as one block.
DepGraph graphOf(const std::vector<uint32_t> &words)
{
    return buildDepGraph(words, Block{0, words.size(), false});
}

size_t edgeCount(const DepGraph &g)
{
    size_t n = 0;
    for (const auto &s : g.succ)
        n += s.size();
    return n;
}

} // namespace

TEST(ReorderDepGraph, RawEdge)
{
    auto g = graphOf({addi(1, 0, 5), addi(2, 1, 1)});
    EXPECT_TRUE(g.hasEdge(0, 1));
    EXPECT_EQ(edgeCount(g), 1u);
}

TEST(ReorderDepGraph, IndependentHaveNoEdge)
{
    auto g = graphOf({addi(1, 0, 1), addi(2, 0, 2)});
    EXPECT_EQ(edgeCount(g), 0u);
}

TEST(ReorderDepGraph, WarEdge)
{
    // add reads x1, then addi overwrites x1: addi must not move above add.
    auto g = graphOf({add(3, 1, 2), addi(1, 0, 9)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderDepGraph, WawEdge)
{
    auto g = graphOf({addi(1, 0, 1), addi(1, 0, 2)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderDepGraph, X0IsNeverADependency)
{
    // Writes to x0 are discarded, so these three are all independent.
    auto g = graphOf({addi(0, 0, 5), addi(0, 0, 6), add(1, 0, 0)});
    EXPECT_EQ(edgeCount(g), 0u);
}

TEST(ReorderDepGraph, ImmediateBitsAreNotARead)
{
    // addi x6,x0,5 has 5 in the rs2 bit field, but it does not read x5.
    // The simulator may stall here (see the quirk test), yet there is no real dependency.
    auto g = graphOf({lw(5, 10, 0), addi(6, 0, 5)});
    EXPECT_FALSE(g.hasEdge(0, 1));
}

TEST(ReorderDepGraph, MemoryOrdering)
{
    EXPECT_TRUE(graphOf({sw(1, 10, 0), lw(2, 11, 0)}).hasEdge(0, 1));   // store then load
    EXPECT_TRUE(graphOf({lw(2, 11, 0), sw(1, 10, 0)}).hasEdge(0, 1));   // load then store
    EXPECT_TRUE(graphOf({sw(1, 10, 0), sw(2, 11, 0)}).hasEdge(0, 1));   // store then store
    EXPECT_FALSE(graphOf({lw(1, 10, 0), lw(2, 11, 0)}).hasEdge(0, 1));  // load then load
}

TEST(ReorderDepGraph, StoreReadsBothRegisters)
{
    // sw x2, 0(x1) reads x1 (address) and x2 (data); addi writes x2 first -> RAW.
    auto g = graphOf({addi(2, 0, 7), sw(2, 1, 0)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderDepGraph, TerminatorStaysLast)
{
    // 0 addi x1; 1 addi x5 (unrelated to the branch); 2 beq x1,x0.
    std::vector<uint32_t> w = {addi(1, 0, 1), addi(5, 0, 2), beq(1, 0, 8)};
    auto blocks = findBlocks(w);
    ASSERT_EQ(blocks.size(), 1u);                 // one block {0,3}; the branch target (index 4) is outside the program
    EXPECT_TRUE(blocks[0].hasTerminator);
    auto g = buildDepGraph(w, blocks[0]);
    EXPECT_TRUE(g.hasEdge(0, 2));
    EXPECT_TRUE(g.hasEdge(1, 2));                 // no register link, yet it must stay before the branch
}

TEST(ReorderDepGraph, BarrierIsOrderedAgainstEverything)
{
    auto g = graphOf({addi(1, 0, 1), kEcall, addi(2, 0, 2)});
    EXPECT_TRUE(g.hasEdge(0, 1));
    EXPECT_TRUE(g.hasEdge(1, 2));
    // 0 and 2 are not linked directly. They stay in order through the ecall: 0 -> 1 -> 2.
    EXPECT_FALSE(g.hasEdge(0, 2));
}

TEST(ReorderDepGraph, PredAndSuccAgree)
{
    auto g = graphOf({addi(1, 0, 1), addi(2, 1, 1), add(3, 1, 2), sw(3, 10, 0)});
    for (size_t i = 0; i < g.size; ++i)
        for (size_t j : g.succ[i])
        {
            EXPECT_LT(i, j);
            bool found = false;
            for (size_t p : g.pred[j])
                found = found || (p == i);
            EXPECT_TRUE(found);
        }
}
