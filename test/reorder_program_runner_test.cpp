#include <gtest/gtest.h>

#include <sstream>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
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
    EXPECT_EQ(hf.dataWords[0], 123u);   // if this fails: stores are not reaching readWord_d (cache not flushed)
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
TEST(ReorderProgramRunner, DiagnoseStoreVisibility)
{
    auto program = assembleText(".data\n"
                                "buf: .word 0\n"
                                ".text\n"
                                "la x5, buf\n"
                                "addi x6, x0, 123\n"
                                "sw x6, 0(x5)\n"
                                "lw x7, 0(x5)\n");
    reorder_detail::CoutSilencer silence;
    auto vm = std::make_unique<Steppable<RV5StageProcessorHF>>();
    vm->LoadProgram(program);
    while (vm->cycle_s_ < 5000 && vm->StepAndNotify())
    {
    }
    uint64_t dataStart = vm_config::config.getDataSectionStart();
    std::cerr << "[DIAG] dataStart=" << dataStart
              << " x5(addr of buf)=" << vm->registers_.ReadGpr(5)
              << " x6=" << vm->registers_.ReadGpr(6)
              << " x7(loaded back)=" << vm->registers_.ReadGpr(7)
              << " readWord(cached path)=" << vm->memory_controller_.readWord(dataStart)
              << " readWord_d(direct path)=" << vm->memory_controller_.readWord_d(dataStart)
              << std::endl;
}
