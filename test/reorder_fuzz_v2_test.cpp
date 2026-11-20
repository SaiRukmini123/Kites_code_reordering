// Tests for the structured program generator and the shared fuzz checks in reorder/fuzz.h, plus
// one negative control per check: a fault injected in test code (into the produced schedule, or
// by choosing the Textbook hazard rule) that the check must report.
// Counts here are small; the large run is `kites-cli reorder-fuzz --generator v2` (docs/TESTING.md).

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "reorder/bench_eval.h"
#include "reorder/fuzz.h"
#include "utils/utils.h"

using namespace Kites;
using namespace Kites::fuzz;
using Kites::reorder::PipelineKind;

namespace
{
constexpr uint64_t kSeed = kDefaultRngSeed;

size_t sum(const std::array<size_t, 6> &a) { return a[0] + a[1] + a[2] + a[3] + a[4] + a[5]; }

FuzzOptions fastOptions(unsigned inputs)
{
    FuzzOptions options;
    options.inputsPerProgram = inputs;
    options.cycleCap = 20000;
    return options;
}

// Static: inside every backward-branch loop, the counter register is written only by the
// decrement right before the back-branch.
bool counterOnlyDecremented(const AssembledProgram &program)
{
    const std::vector<uint32_t> &w = program.text_buffer;
    for (size_t k = 0; k < w.size(); ++k)
    {
        if ((w[k] & 0x7F) != 0x63 || reorder::branchImm(w[k]) >= 0)
        {
            continue;
        }
        const size_t head = static_cast<size_t>(static_cast<int64_t>(k) + reorder::branchImm(w[k]) / 4);
        for (size_t i = head; i < k; ++i)
        {
            const reorder::DecodedInst d = reorder::decode(w[i]);
            if (reorder::writesReg(d) && d.rd == kStructuredCounterReg && i + 1 != k)
            {
                return false;
            }
        }
    }
    return true;
}

// Structured programs (from the default seed) that have a feature, for the negative controls.
template <class Pred>
std::vector<FuzzProgram> structuredWith(size_t want, Pred pred, size_t scan = 60)
{
    std::vector<FuzzProgram> out;
    for (size_t i = 0; i < scan && out.size() < want; ++i)
    {
        FuzzProgram p = structuredProgram(kSeed, i);
        if (pred(p))
        {
            out.push_back(std::move(p));
        }
    }
    return out;
}

// Blocks of the program, for finding pairs inside one block.
std::vector<reorder::Block> blocksOf(const AssembledProgram &program)
{
    return reorder::findBlocks(program.text_buffer, reorder::labelLeaders(program));
}

bool isPlain(uint32_t w)
{
    const reorder::DecodedInst d = reorder::decode(w);
    return !d.is_branch && !d.is_jump && (w & 0x7F) != 0x17 && (w & 0x7F) != 0x73;
}

// Byte range [lo, hi) of a load/store relative to its base register.
std::pair<int64_t, int64_t> accessRange(uint32_t w)
{
    const uint32_t opcode = w & 0x7F;
    const int64_t imm = opcode == 0x23 ? static_cast<int64_t>(((static_cast<int32_t>(w) >> 25) << 5) | ((w >> 7) & 0x1F))
                                       : static_cast<int64_t>(static_cast<int32_t>(w) >> 20);
    const int64_t size = int64_t{1} << ((w >> 12) & 0x3);
    return {imm, imm + size};
}

// First (store, later overlapping load through the same base) pair in one block, with no write
// to the base between them; {0, 0} if none.
std::pair<size_t, size_t> storeThenOverlappingLoad(const AssembledProgram &program)
{
    const std::vector<uint32_t> &w = program.text_buffer;
    for (const reorder::Block &b : blocksOf(program))
    {
        for (size_t i = b.begin; i < b.end; ++i)
        {
            if ((w[i] & 0x7F) != 0x23)
            {
                continue;
            }
            const uint32_t base = (w[i] >> 15) & 0x1F;
            const auto [slo, shi] = accessRange(w[i]);
            for (size_t j = i + 1; j < b.end; ++j)
            {
                const reorder::DecodedInst d = reorder::decode(w[j]);
                if ((w[j] & 0x7F) == 0x03 && ((w[j] >> 15) & 0x1F) == base)
                {
                    const auto [llo, lhi] = accessRange(w[j]);
                    if (llo < shi && slo < lhi)
                    {
                        return {i, j};
                    }
                }
                if (reorder::writesReg(d) && d.rd == base)
                {
                    break;
                }
            }
        }
    }
    return {0, 0};
}

// First adjacent pair (i, i+1) in one block where i+1 writes a register i reads (WAR) and the
// two have no other dependence; {0, 0} if none.
std::pair<size_t, size_t> adjacentWarPair(const AssembledProgram &program)
{
    const std::vector<uint32_t> &w = program.text_buffer;
    for (const reorder::Block &b : blocksOf(program))
    {
        for (size_t i = b.begin; i + 1 < b.end; ++i)
        {
            if (!isPlain(w[i]) || !isPlain(w[i + 1]))
            {
                continue;
            }
            const reorder::DecodedInst a = reorder::decode(w[i]);
            const reorder::DecodedInst c = reorder::decode(w[i + 1]);
            if (!reorder::writesReg(c) || c.rd == 0)
            {
                continue;
            }
            const bool war = (a.reads_rs1 && a.rs1 == c.rd) || (a.reads_rs2 && a.rs2 == c.rd);
            const bool raw = reorder::writesReg(a) && ((c.reads_rs1 && c.rs1 == a.rd) || (c.reads_rs2 && c.rs2 == a.rd));
            const bool waw = reorder::writesReg(a) && a.rd == c.rd;
            const bool memory = (a.is_load || a.is_store) && (c.is_load || c.is_store);
            if (war && !raw && !waw && !memory)
            {
                return {i, i + 1};
            }
        }
    }
    return {0, 0};
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The generator
// ---------------------------------------------------------------------------------------------

TEST(ReorderFuzzV2, StructuredProgramsAreReproducible)
{
    setupVmStateDirectory();
    const FuzzProgram a = structuredProgram(kSeed, 3);
    const FuzzProgram b = structuredProgram(kSeed, 3);
    EXPECT_EQ(a.source, b.source);
    EXPECT_EQ(a.program.text_buffer, b.program.text_buffer);
    EXPECT_NE(a.source, structuredProgram(kSeed, 4).source);
    EXPECT_NE(a.source, structuredProgram(kSeed + 1, 3).source);
    EXPECT_EQ(structuredPrograms(5, kSeed)[3].source, a.source);
}

// The mix is read back from the assembled words (programMix), not assumed.
TEST(ReorderFuzzV2, StructuredProgramsContainEveryFeature)
{
    setupVmStateDirectory();
    ProgramMix total;
    size_t programsWithLargeBlock = 0;
    for (const FuzzProgram &p : structuredPrograms(40, kSeed))
    {
        const ProgramMix m = programMix(p.program);
        for (size_t k = 0; k < 6; ++k)
        {
            total.forwardBranches[k] += m.forwardBranches[k];
        }
        for (size_t k = 0; k < 7; ++k)
        {
            total.loads[k] += m.loads[k];
        }
        for (size_t k = 0; k < 4; ++k)
        {
            total.stores[k] += m.stores[k];
        }
        for (size_t k = 0; k < 5; ++k)
        {
            total.edgeImmediates[k] += m.edgeImmediates[k];
            total.edgeShifts[k] += m.edgeShifts[k];
        }
        total.forwardJals += m.forwardJals;
        total.backwardBranches += m.backwardBranches;
        total.eligibleLoops += m.eligibleLoops;
        total.loopsWithBranch += m.loopsWithBranch;
        total.auipcMidBlock += m.auipcMidBlock;
        total.mulDiv += m.mulDiv;
        total.wMulDiv += m.wMulDiv;
        total.quirkImmediates += m.quirkImmediates;
        total.largestBlock = std::max(total.largestBlock, m.largestBlock);
        total.blocksOf30 += m.blocksOf30;
        programsWithLargeBlock += m.blocksOf30 > 0 ? 1 : 0;
    }
    const char *branchNames[] = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};
    for (size_t k = 0; k < 6; ++k)
    {
        EXPECT_GT(total.forwardBranches[k], 0u) << branchNames[k];
    }
    const char *loadNames[] = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu"};
    for (size_t k = 0; k < 7; ++k)
    {
        EXPECT_GT(total.loads[k], 0u) << loadNames[k];
    }
    const char *storeNames[] = {"sb", "sh", "sw", "sd"};
    for (size_t k = 0; k < 4; ++k)
    {
        EXPECT_GT(total.stores[k], 0u) << storeNames[k];
    }
    const int edges[] = {0, 1, -1, 2047, -2048};
    const int shifts[] = {0, 1, 31, 32, 63};
    for (size_t k = 0; k < 5; ++k)
    {
        EXPECT_GT(total.edgeImmediates[k], 0u) << "immediate " << edges[k];
        EXPECT_GT(total.edgeShifts[k], 0u) << "shift " << shifts[k];
    }
    EXPECT_GT(total.forwardJals, 0u);
    EXPECT_GT(total.backwardBranches, 0u);
    EXPECT_GT(total.eligibleLoops, 0u);
    EXPECT_GT(total.loopsWithBranch, 0u);
    EXPECT_GT(total.auipcMidBlock, 0u);
    EXPECT_GT(total.mulDiv, 0u);
    EXPECT_GT(total.wMulDiv, 0u);
    EXPECT_GT(total.quirkImmediates, 0u);
    EXPECT_GT(programsWithLargeBlock, 0u) << "some block of 30-64 instructions";
    std::cout << "[ MIX ] 40 programs: forward branches " << sum(total.forwardBranches) << ", jal "
              << total.forwardJals << ", loops " << total.backwardBranches << " (" << total.eligibleLoops
              << " single-block, " << total.loopsWithBranch << " with a branch inside), la inside a block "
              << total.auipcMidBlock << ", M " << total.mulDiv << ", W " << total.wMulDiv << ", quirk immediates "
              << total.quirkImmediates << ", blocks of 30-64 " << total.blocksOf30 << " in "
              << programsWithLargeBlock << " programs, largest block " << total.largestBlock << std::endl;
}

TEST(ReorderFuzzV2, StructuredProgramsAreValidAndTerminate)
{
    setupVmStateDirectory();
    bench_eval::ProgramHeader header;
    for (const FuzzProgram &p : structuredPrograms(25, kSeed))
    {
        Seed unused;
        for (const std::string &problem : bench_eval::staticValidityProblems(p.program, header, unused))
        {
            // Header rules are for files in bench/programs; everything else applies.
            EXPECT_EQ(problem.rfind("header:", 0), 0u) << p.name << ": " << problem << "\n" << p.source;
        }
        const RunResult hf = runProgram<RV5StageProcessorHF>(p.program, p.declared, bench_eval::kBenchCycleCap);
        const RunResult hnf = runProgram<RV5StageProcessorHNF>(p.program, p.declared, bench_eval::kBenchCycleCap);
        EXPECT_TRUE(bench_eval::dynamicValidityProblems(hf, hnf).empty()) << p.name << "\n" << p.source;
        EXPECT_TRUE(counterOnlyDecremented(p.program)) << p.name;
        EXPECT_TRUE(bench_eval::assemblerHangLabel(p.source).empty()) << p.name;

        // Random inputs never touch the counter or the bases' addresses, and memory stays random.
        const InputPlan plan = analyzeInputs(p.program, p.declared);
        EXPECT_TRUE(plan.fixedRegister[kStructuredCounterReg]) << p.name;
        EXPECT_FALSE(plan.memoryFeedsAddress) << p.name;
        const std::array<bool, 32> liveIn = possiblyLiveIn(p.program);
        for (int base : kStructuredBaseRegs)
        {
            EXPECT_TRUE(!liveIn[static_cast<size_t>(base)] || plan.fixedRegister[static_cast<size_t>(base)])
                << p.name << " x" << base;
        }
        // And the program still finishes on random inputs.
        for (size_t input = 0; input < 2; ++input)
        {
            const Seed seed = randomSeed(p, plan, inputSeedFor(kSeed, 0, input));
            EXPECT_FALSE(runProgram<RV5StageProcessorHNF>(p.program, seed, bench_eval::kBenchCycleCap).hitCap)
                << p.name;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The checks, on new and old programs (small counts)
// ---------------------------------------------------------------------------------------------

TEST(ReorderFuzzV2, StructuredProgramsPassEveryCheck)
{
    setupVmStateDirectory();
    FuzzOptions options = fastOptions(3);
    options.checkReplay = true;
    options.checkUnroll = true;
    const FuzzReport report = fuzzPrograms(structuredPrograms(10, kSeed), options);
    std::cout << summaryText(report);
    EXPECT_TRUE(fuzzClean(report));
    size_t unrolled = 0;
    for (const FuzzPipelineStats &s : report.stats)
    {
        EXPECT_EQ(s.passes + s.mismatches + s.capHits + s.invalidInputs, s.runs);
        EXPECT_EQ(s.declaredRuns, 10u);
        EXPECT_EQ(s.blockChecks, 10u);
        EXPECT_GT(s.replayChecks, 0u);
        EXPECT_EQ(s.unrollTried, 10u);
        unrolled += s.unrolledPrograms;
    }
    EXPECT_GT(unrolled, 0u) << "the unroll check must be exercised";
}

TEST(ReorderFuzzV2, OriginalGeneratedProgramsPassEveryCheck)
{
    setupVmStateDirectory();
    FuzzOptions options = fastOptions(3);
    options.checkReplay = true;
    options.checkUnroll = true;
    const FuzzReport report = fuzzPrograms(randomPrograms(10, kSeed), options);
    EXPECT_TRUE(fuzzClean(report)) << summaryText(report);
    for (const FuzzPipelineStats &s : report.stats)
    {
        EXPECT_GT(s.replayChecks, 0u);
        EXPECT_EQ(s.replayNotJudged, 0u);
        EXPECT_EQ(s.unrolledPrograms, 0u); // straight-line: nothing to unroll
    }
}

TEST(ReorderFuzzV2, BlockInvariantsAcceptTheLibrarysSchedules)
{
    setupVmStateDirectory();
    for (const FuzzProgram &p : structuredPrograms(15, kSeed))
    {
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            for (reorder::ScheduleMode mode : {reorder::ScheduleMode::Greedy, reorder::ScheduleMode::Optimal})
            {
                const reorder::ScheduleReport r = reorder::scheduleProgram(
                    p.program.text_buffer, kind, reorder::labelLeaders(p.program), true, mode);
                const std::vector<std::string> problems = blockInvariantProblems(p.program, r.words);
                EXPECT_TRUE(problems.empty()) << p.name << ": " << (problems.empty() ? "" : problems.front());
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Negative controls: each check must report the fault injected into it
// ---------------------------------------------------------------------------------------------

// 1. A pinned auipc moved by one slot: the block-invariant check reports it.
TEST(ReorderFuzzNegativeControl, MovedAuipcIsCaughtByTheBlockCheck)
{
    setupVmStateDirectory();
    const std::vector<FuzzProgram> programs =
        structuredWith(5, [](const FuzzProgram &p) { return programMix(p.program).auipcMidBlock > 0; });
    ASSERT_FALSE(programs.empty());
    size_t moved = 0;
    const FuzzReport report = fuzzPrograms(
        programs, fastOptions(2),
        [&moved](const FuzzProgram &p, PipelineKind, std::vector<uint32_t> words)
        {
            std::vector<bool> blockStart(words.size(), false);
            for (const reorder::Block &b : blocksOf(p.program))
            {
                blockStart[b.begin] = true;
            }
            for (size_t k = 1; k < words.size(); ++k)
            {
                if ((words[k] & 0x7F) == 0x17 && !blockStart[k])
                {
                    std::swap(words[k - 1], words[k]); // one slot earlier, same block
                    ++moved;
                    break;
                }
            }
            return words;
        });
    ASSERT_EQ(moved, 2 * programs.size());
    EXPECT_FALSE(fuzzClean(report));
    size_t auipcReports = 0;
    for (const FuzzProblem &v : report.invariantViolations)
    {
        auipcReports += v.detail.find("auipc") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(auipcReports, moved) << summaryText(report);
    std::cout << "[ CONTROL ] moved auipc: " << auipcReports << "/" << moved
              << " caught by the block check; state check saw " << report.mismatches.size() << " mismatching input(s)"
              << std::endl;
}

// 2. A store swapped with a later overlapping load: the final-state comparison reports it.
TEST(ReorderFuzzNegativeControl, StoreSwappedWithOverlappingLoadIsCaughtByTheStateCheck)
{
    setupVmStateDirectory();
    const std::vector<FuzzProgram> programs = structuredWith(
        5, [](const FuzzProgram &p) { return storeThenOverlappingLoad(p.program).second != 0; });
    ASSERT_FALSE(programs.empty());
    std::set<std::string> caught;
    const FuzzReport report = fuzzPrograms(programs, fastOptions(4),
                                           [](const FuzzProgram &p, PipelineKind, std::vector<uint32_t>)
                                           {
                                               std::vector<uint32_t> words = p.program.text_buffer;
                                               const auto [store, load] = storeThenOverlappingLoad(p.program);
                                               std::swap(words[store], words[load]);
                                               return words;
                                           });
    for (const FuzzMismatch &m : report.mismatches)
    {
        caught.insert(m.program);
    }
    EXPECT_FALSE(report.mismatches.empty()) << summaryText(report);
    EXPECT_TRUE(report.invariantViolations.empty()) << "a swap inside a block is still a permutation";
    std::cout << "[ CONTROL ] store/load swap: " << caught.size() << "/" << programs.size()
              << " programs caught by the state check (" << report.mismatches.size() << " mismatching inputs)"
              << std::endl;
}

// 3. A WAR pair swapped: the final-state comparison reports it.
TEST(ReorderFuzzNegativeControl, SwappedWarPairIsCaughtByTheStateCheck)
{
    setupVmStateDirectory();
    const std::vector<FuzzProgram> programs =
        structuredWith(5, [](const FuzzProgram &p) { return adjacentWarPair(p.program).second != 0; });
    ASSERT_FALSE(programs.empty());
    std::set<std::string> caught;
    const FuzzReport report = fuzzPrograms(programs, fastOptions(4),
                                           [](const FuzzProgram &p, PipelineKind, std::vector<uint32_t>)
                                           {
                                               std::vector<uint32_t> words = p.program.text_buffer;
                                               const auto [reader, writer] = adjacentWarPair(p.program);
                                               std::swap(words[reader], words[writer]);
                                               return words;
                                           });
    for (const FuzzMismatch &m : report.mismatches)
    {
        caught.insert(m.program);
    }
    EXPECT_FALSE(report.mismatches.empty()) << summaryText(report);
    std::cout << "[ CONTROL ] WAR swap: " << caught.size() << "/" << programs.size()
              << " programs caught by the state check (" << report.mismatches.size() << " mismatching inputs)"
              << std::endl;
}

// 4. The replay with the Textbook hazard rule (no stalls on immediate bits): the replay check
// reports a difference from the VM.
TEST(ReorderFuzzNegativeControl, TextbookHazardRuleIsCaughtByTheReplayCheck)
{
    setupVmStateDirectory();
    FuzzOptions options = fastOptions(2);
    options.checkReplay = true;
    options.replayModel = reorder::HazardModel::Textbook;
    const std::vector<FuzzProgram> programs =
        structuredWith(6, [](const FuzzProgram &p) { return programMix(p.program).quirkImmediates > 0; });
    ASSERT_FALSE(programs.empty());
    const FuzzReport report = fuzzPrograms(programs, options);
    EXPECT_FALSE(report.replayMismatches.empty());
    EXPECT_TRUE(report.mismatches.empty()) << "the programs themselves are fine";
    size_t predictedFewer = 0;
    for (const ReplayMismatch &m : report.replayMismatches)
    {
        predictedFewer += m.predictedStalls < m.vmStalls ? 1 : 0;
    }
    EXPECT_EQ(predictedFewer, report.replayMismatches.size()) << "the Textbook rule only drops stalls";
    std::cout << "[ CONTROL ] textbook replay: " << report.replayMismatches.size() << " of "
              << report.stats[0].replayChecks + report.stats[1].replayChecks << " replay checks disagree with the VM"
              << std::endl;

    // The same programs with the exact rule agree with the VM everywhere.
    options.replayModel = reorder::HazardModel::Exact;
    EXPECT_TRUE(fuzzPrograms(programs, options).replayMismatches.empty());
}
