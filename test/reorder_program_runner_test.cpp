#include <gtest/gtest.h>

#include <sstream>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "reorder/program_runner.h"
#include "utils/utils.h"

using namespace Kites;

namespace
{
AssembledProgram assembleText(const std::string &source)
{
    setupVmStateDirectory();
    std::istringstream stream(source);
    return assemble(stream);
}
} // namespace

// A: branch and jump bubbles must NOT count as stalls.
TEST(ReorderProgramRunner, TakenBranchIsNotCountedAsStall)
{
    auto program = assembleText("addi x1, x0, 1\n"
                                "addi x4, x0, 2\n"
                                "beq x0, x0, skip\n"     // always taken, reads only x0
                                "addi x2, x0, 5\n"
                                "skip:\n"
                                "addi x3, x0, 7\n");
    auto hf = runProgram<RV5StageProcessorHF>(program, {}, 5000);
    auto hnf = runProgram<RV5StageProcessorHNF>(program, {}, 5000);
    EXPECT_EQ(hf.stalls, 0u);
    EXPECT_EQ(hnf.stalls, 0u);
    std::cerr << "[CHECK] hf cycles=" << hf.cycles << " retired=" << hf.retired
              << " (extra cycles beyond retired+4 are branch bubbles, which must not be in stalls)\n";
}

// B: a store must show up in the data window we compare.
TEST(ReorderProgramRunner, StoreIsVisibleInDataWindow)
{
    auto program = assembleText(".data\n"
                                "buf: .word 0\n"
                                ".text\n"
                                "la x5, buf\n"
                                "addi x6, x0, 123\n"
                                "sw x6, 0(x5)\n");
    auto hf = runProgram<RV5StageProcessorHF>(program, {}, 5000);
    ASSERT_FALSE(hf.dataWords.empty());
    EXPECT_EQ(hf.regs[5], vm_config::config.getDataSectionStart()); // la must point at buf
    EXPECT_EQ(hf.dataWords[0], 123u);
}

// C: swapping in the same words must change nothing.
TEST(ReorderProgramRunner, IdentityWithTextIsEquivalent)
{
    auto program = assembleText("addi x1, x0, 100\n"
                                "addi x2, x0, 20\n"
                                "add x6, x2, x1\n");
    auto a = runProgram<RV5StageProcessorHF>(program, {}, 5000);
    auto b = runProgram<RV5StageProcessorHF>(withText(program, program.text_buffer), {}, 5000);
    EXPECT_TRUE(sameState(a, b));
    EXPECT_EQ(a.cycles, b.cycles);
}
// D: a store then load through the cached path agrees with the direct path the runner reads.
TEST(ReorderProgramRunner, StoreAndLoadAgreeWithDataWindow)
{
    auto program = assembleText(".data\n"
                                "buf: .word 0\n"
                                ".text\n"
                                "la x5, buf\n"
                                "addi x6, x0, 123\n"
                                "sw x6, 0(x5)\n"
                                "lw x7, 0(x5)\n");
    auto hf = runProgram<RV5StageProcessorHF>(program, {}, 5000);
    EXPECT_EQ(hf.regs[7], 123u);
    EXPECT_EQ(hf.dataWords[0], 123u);
}

// E: AUIPC adds the sign-extended upper immediate to its own PC, on every pipeline variant.
template <class VM> void expectAuipc()
{
    auto program = assembleText("addi x1, x0, 1\n"
                                "auipc x5, 0x10000\n"  // pc=4 -> 0x10000004
                                "auipc x6, 0x80000\n"  // pc=8, bit 31 set -> sign-extended
                                "auipc x7, 0\n");      // pc=12 -> 12
    auto r = runProgram<VM>(program, {}, 5000);
    ASSERT_FALSE(r.hitCap);
    EXPECT_EQ(r.regs[5], 0x10000004ull);
    EXPECT_EQ(r.regs[6], 0xFFFFFFFF80000008ull);
    EXPECT_EQ(r.regs[7], 12ull);
}
TEST(ReorderProgramRunner, AuipcHF) { expectAuipc<RV5StageProcessorHF>(); }
TEST(ReorderProgramRunner, AuipcHNF) { expectAuipc<RV5StageProcessorHNF>(); }
TEST(ReorderProgramRunner, AuipcNHF) { expectAuipc<RV5StageProcessorNHF>(); }
TEST(ReorderProgramRunner, AuipcNHNF) { expectAuipc<RV5StageProcessorNHNF>(); }
