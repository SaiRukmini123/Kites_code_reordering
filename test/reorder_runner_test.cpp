#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <sstream>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/program_runner.h"
#include "utils/utils.h"

using namespace Kites;

namespace
{
struct NullBuf : std::streambuf { int overflow(int c) override { return c; } };
struct Silence
{
    NullBuf nb; std::streambuf *old;
    Silence() { old = std::cout.rdbuf(&nb); }
    ~Silence() { std::cout.rdbuf(old); }
};
AssembledProgram asmText(const std::string &s)
{
    std::istringstream in(s);
    return assemble(in);
}
// Reordered words: swap instruction i and j of the original.
std::vector<uint32_t> swapped(const AssembledProgram &p, size_t i, size_t j)
{
    std::vector<uint32_t> w(p.text_buffer.begin(), p.text_buffer.end());
    std::swap(w[i], w[j]);
    return w;
}
const char *kProg = "addi x1, x0, 5\naddi x2, x0, 7\nadd x3, x1, x2\n";
} // namespace

TEST(RunnerTest, IdenticalWordsGiveIdenticalResult)
{
    setupVmStateDirectory();
    Silence s;
    auto prog = asmText(kProg);
    std::vector<uint32_t> same(prog.text_buffer.begin(), prog.text_buffer.end());
    auto a = runProgram<RV5StageProcessorHF>(prog, Seed{}, 10000);
    auto b = runProgram<RV5StageProcessorHF>(withText(prog, same), Seed{}, 10000);
    EXPECT_TRUE(sameState(a, b));
    EXPECT_EQ(a.cycles, b.cycles);
    EXPECT_EQ(a.regs[3], 12u);
}

TEST(RunnerTest, IndependentSwapKeepsState)
{
    setupVmStateDirectory();
    Silence s;
    auto prog = asmText(kProg);
    auto a = runProgram<RV5StageProcessorHF>(prog, Seed{}, 10000);
    auto b = runProgram<RV5StageProcessorHF>(withText(prog, swapped(prog, 0, 1)), Seed{}, 10000);
    EXPECT_TRUE(sameState(a, b));
}

TEST(RunnerTest, DependentSwapIsDetected)
{
    setupVmStateDirectory();
    Silence s;
    auto prog = asmText(kProg);
    auto a = runProgram<RV5StageProcessorHF>(prog, Seed{}, 10000);
    auto b = runProgram<RV5StageProcessorHF>(withText(prog, swapped(prog, 1, 2)), Seed{}, 10000);
    EXPECT_FALSE(sameState(a, b)); // x3 becomes 5 instead of 12
}

TEST(RunnerTest, CapStopsInfiniteLoop)
{
    setupVmStateDirectory();
    Silence s;
    auto prog = asmText("loop:\nbeq x0, x0, loop\n");
    auto r = runProgram<RV5StageProcessorHF>(prog, Seed{}, 5000);
    EXPECT_TRUE(r.hitCap);
}