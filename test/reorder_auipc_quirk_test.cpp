#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

namespace
{

struct Observed
{
    uint64_t x10 = 0;
    uint64_t x5 = 0;
};

template <typename VmType>
Observed runSource(const std::string &source)
{
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<VmType>();
    vm->LoadProgram(program);
    vm->DebugRun();
    return {vm->registers_.ReadGpr(10), vm->registers_.ReadGpr(5)};
}

// auipc x10, 1 at address 4: correct result is 4 + (1 << 12) = 0x1004.
const char *kAuipcSource = "addi x1, x0, 1\nauipc x10, 1\n";

// la x10, value expands to auipc + addi; value is the first data word, at
// the data section start 0x10000000, so x10 should be 0x10000000 and the
// load should return 1234.
const char *kLaSource = ".data\nvalue: .word 1234\n.text\nla x10, value\nlw x5, 0(x10)\n";

template <typename VmType>
void report(const char *vmName, Observed &auipc, Observed &la)
{
    auipc = runSource<VmType>(kAuipcSource);
    la = runSource<VmType>(kLaSource);
    std::cout << "[AUIPC-QUIRK] " << vmName << ": auipc x10, 1 @0x4 -> x10=0x" << std::hex
              << auipc.x10 << " (expected 0x1004) | la x10, value -> x10=0x" << la.x10
              << " (expected 0x10000000), lw -> x5=" << std::dec << la.x5 << " (expected 1234)"
              << std::endl;
}

} // namespace

// Asserts what Kites actually does today (this is a finding, not a fix):
// the single-cycle RVSS VM is correct; both pipelined VMs leave auipc's
// destination at 0, so la yields 0 and the load reads the program's own
// first instruction word (auipc x10, 65536 = 0x10000517) from address 0.
TEST(ReorderAuipcQuirk, ObservedResults)
{
    Observed hfAuipc, hfLa, hnfAuipc, hnfLa, ssAuipc, ssLa;
    report<Kites::RV5StageProcessorHF>("rv5s h_f ", hfAuipc, hfLa);
    report<Kites::RV5StageProcessorHNF>("rv5s h_nf", hnfAuipc, hnfLa);
    report<Kites::RVSSProcessor>("rvss     ", ssAuipc, ssLa);

    EXPECT_EQ(ssAuipc.x10, 0x1004u);
    EXPECT_EQ(ssLa.x10, 0x10000000u);
    EXPECT_EQ(ssLa.x5, 1234u);

    for (const Observed *o : {&hfAuipc, &hnfAuipc})
    {
        EXPECT_EQ(o->x10, 0u);
    }
    for (const Observed *o : {&hfLa, &hnfLa})
    {
        EXPECT_EQ(o->x10, 0u);
        EXPECT_EQ(o->x5, 0x10000517u);
    }
}
