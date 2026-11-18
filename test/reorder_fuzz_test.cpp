// Tests for reorder/fuzz.h: which inputs may be randomized, reproducibility, fast fuzz runs over
// the benchmark set and generated programs, and a negative control.
// The full run is `kites-cli reorder-fuzz` (20 inputs per program by default).
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "reorder/fuzz.h"
#include "reorder_hazard_cases.h"

using namespace Kites;
using namespace Kites::fuzz;
using Kites::reorder::PipelineKind;

namespace
{
constexpr unsigned kFastInputs = 2;
// The longest benchmark run is under 2000 cycles; a lower cap than reorder-bench's keeps the few
// inputs that make a data-driven loop run away (reported as cap hits) cheap.
constexpr unsigned kFastCycleCap = 20000;

FuzzProgram makeProgram(const std::string &name, const std::string &source, const Seed &declared = {})
{
    return {name, hazard_bench::assembleHazardText(source), declared};
}

// Swaps the first load with the first instruction that uses its result (the negative control):
// the consumer then reads the register before the load has written it.
std::vector<uint32_t> swapLoadAndUse(std::vector<uint32_t> words)
{
    for (size_t i = 0; i < words.size(); ++i)
    {
        const reorder::DecodedInst p = reorder::decode(words[i]);
        if (!p.is_load || !reorder::writesReg(p))
        {
            continue;
        }
        for (size_t j = i + 1; j < words.size(); ++j)
        {
            const reorder::DecodedInst c = reorder::decode(words[j]);
            if ((c.reads_rs1 && c.rs1 == p.rd) || (c.reads_rs2 && c.rs2 == p.rd))
            {
                std::swap(words[i], words[j]);
                return words;
            }
        }
    }
    return words;
}
} // namespace

TEST(ReorderFuzz, DeclaredAndLiveInAddressRegistersStayFixed)
{
    Seed declared;
    declared.regs = {{10, 0x10000000}};
    FuzzProgram p = makeProgram("t", "lw x5, 0(x11)\nadd x6, x5, x7\nlw x8, 4(x10)\n", declared);
    const InputPlan plan = analyzeInputs(p.program, p.declared);
    EXPECT_TRUE(plan.fixedRegister[0]);
    EXPECT_TRUE(plan.fixedRegister[10]);  // declared input
    EXPECT_TRUE(plan.fixedRegister[11]);  // read before written, used as a base
    EXPECT_FALSE(plan.fixedRegister[7]);  // read before written, but only as data
    EXPECT_FALSE(plan.fixedRegister[5]);  // written before it is read
    EXPECT_FALSE(plan.memoryFeedsAddress);
}

TEST(ReorderFuzz, AddressTaintFollowsArithmetic)
{
    FuzzProgram p = makeProgram("t", "add x12, x11, x13\nlw x5, 0(x12)\n");
    const InputPlan plan = analyzeInputs(p.program, p.declared);
    EXPECT_TRUE(plan.fixedRegister[11]);
    EXPECT_TRUE(plan.fixedRegister[13]);
    EXPECT_FALSE(plan.fixedRegister[12]); // written before it is read
}

TEST(ReorderFuzz, PointerSetByLaIsRandomizedHarmlessly)
{
    FuzzProgram p = makeProgram("t", ".data\nv: .word 1, 2\n.text\nla a0, v\nlw t0, 0(a0)\nadd t1, t0, t0\n");
    const InputPlan plan = analyzeInputs(p.program, p.declared);
    EXPECT_FALSE(plan.fixedRegister[10]); // la writes a0 before any use
    EXPECT_FALSE(plan.memoryFeedsAddress);
}

TEST(ReorderFuzz, LoadedIndexKeepsProgramData)
{
    FuzzProgram p = makeProgram("t", ".data\nv: .word 1, 2\n.text\nla a0, v\nlw t0, 0(a0)\nslli t0, t0, 2\n"
                                     "add t1, a0, t0\nlw t2, 0(t1)\n");
    const InputPlan plan = analyzeInputs(p.program, p.declared);
    EXPECT_TRUE(plan.memoryFeedsAddress);
    const Seed seed = randomSeed(p, plan, 1);
    const uint64_t dataStart = vm_config::config.getDataSectionStart();
    for (const auto &[address, value] : seed.memWords)
    {
        (void)value;
        EXPECT_GE(address, dataStart + 8) << "program data must keep its values";
    }
}

TEST(ReorderFuzz, InputsAreReproducible)
{
    FuzzProgram p = makeProgram("t", "add x6, x5, x7\n");
    const InputPlan plan = analyzeInputs(p.program, p.declared);
    const uint64_t a = inputSeedFor(42, 3, 7);
    EXPECT_EQ(a, inputSeedFor(42, 3, 7));
    EXPECT_NE(a, inputSeedFor(42, 3, 8));
    EXPECT_NE(a, inputSeedFor(43, 3, 7));

    const Seed s1 = randomSeed(p, plan, a), s2 = randomSeed(p, plan, a);
    EXPECT_EQ(s1.regs, s2.regs);
    EXPECT_EQ(s1.memWords, s2.memWords);
    for (const auto &[reg, value] : s1.regs)
    {
        (void)value;
        EXPECT_GE(reg, 1);
        EXPECT_LE(reg, 31);
    }
    EXPECT_EQ(s1.memWords.size(), reorder_report::kDataWindowWords);
}

// Fast version of `kites-cli reorder-fuzz` over the committed benchmark set.
TEST(ReorderFuzz, BenchmarkProgramsKeepTheirStateOnRandomInputs)
{
    const std::vector<FuzzProgram> programs = benchmarkPrograms("../bench/programs");
    ASSERT_GE(programs.size(), 50u);
    FuzzOptions options;
    options.inputsPerProgram = kFastInputs;
    options.cycleCap = kFastCycleCap;
    const FuzzReport report = fuzzPrograms(programs, options);
    std::cout << summaryText(report);
    EXPECT_TRUE(report.mismatches.empty());
    for (const FuzzPipelineStats &s : report.stats)
    {
        EXPECT_EQ(s.runs, programs.size() * kFastInputs);
        EXPECT_EQ(s.passes + s.mismatches + s.capHits + s.invalidInputs, s.runs);
        EXPECT_GT(s.passes, s.runs / 2);
    }
}

TEST(ReorderFuzz, GeneratedProgramsKeepTheirStateOnRandomInputs)
{
    const std::vector<FuzzProgram> programs = randomPrograms(15, kDefaultRngSeed);
    FuzzOptions options;
    options.inputsPerProgram = 3;
    const FuzzReport report = fuzzPrograms(programs, options);
    EXPECT_TRUE(report.mismatches.empty()) << summaryText(report);
    for (const FuzzPipelineStats &s : report.stats)
    {
        EXPECT_EQ(s.passes, s.runs); // straight-line: never hits the cap
    }
    EXPECT_GT(report.reorderedPrograms[1], 0u);
}

// Negative control: break the reordered program on purpose; the fuzzer must catch it and say where.
TEST(ReorderFuzz, NegativeControlReportsMismatch)
{
    const std::vector<FuzzProgram> programs = {
        makeProgram("load_use", ".data\nv: .word 7\n.text\nla a0, v\nlw t0, 0(a0)\nadd t1, t0, t0\n"
                                "addi t2, x0, 32\nsw t1, 0(a0)\n")};
    FuzzOptions options;
    options.inputsPerProgram = 3;
    const FuzzReport report = fuzzPrograms(
        programs, options,
        [](const FuzzProgram &, PipelineKind, std::vector<uint32_t> words) { return swapLoadAndUse(words); });
    std::cout << summaryText(report);
    ASSERT_FALSE(report.mismatches.empty());
    EXPECT_EQ(report.stats[0].mismatches + report.stats[1].mismatches, report.mismatches.size());
    const FuzzMismatch &m = report.mismatches.front();
    EXPECT_EQ(m.program, "load_use");
    EXPECT_EQ(m.rngSeed, options.rngSeed);
    EXPECT_EQ(m.inputSeed, inputSeedFor(options.rngSeed, 0, m.input));
    EXPECT_FALSE(m.detail.empty());
    EXPECT_NE(summaryText(report).find("MISMATCH load_use"), std::string::npos);
}
