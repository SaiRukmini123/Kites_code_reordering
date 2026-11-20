// Focused tests for decision paths of the reorder library that the rest of the suite did not
// reach (found with the coverage build, see docs/TESTING.md): safety-net rejections, optimizer
// budget aborts, unsigned branches in the path replay, jumps in the unroll prediction, the
// unroll rejection and verification gates, and the stack window of the state comparison.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/dep_graph.h"
#include "reorder/fuzz.h"
#include "reorder/loop_unroll.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder/unroll_predict.h"
#include "reorder/vm_safety_net.h"
#include "utils/utils.h"

using namespace Kites::reorder;

namespace
{
constexpr unsigned kCap = 200000;

Kites::AssembledProgram assembleText(const std::string &source)
{
    std::istringstream s(source);
    return Kites::assemble(s);
}

template <class VM>
Kites::RunResult runOn(const Kites::AssembledProgram &program, unsigned cap = kCap, const Kites::Seed &seed = {})
{
    return Kites::runProgram<VM>(program, seed, cap, Kites::reorder_report::kDataWindowWords);
}

// A ScheduleReport that proposes `words` for the whole program, block by block (the shape
// applyWithVmCheck() reads), whatever the scheduler would have chosen.
ScheduleReport proposal(const Kites::AssembledProgram &program, const std::vector<uint32_t> &words)
{
    ScheduleReport report;
    report.words = words;
    for (const Block &b : findBlocks(program.text_buffer, labelLeaders(program)))
    {
        BlockStalls s;
        s.block = b;
        report.blocks.push_back(s);
    }
    return report;
}

// Every edge of the block's dependency graph goes forward in `order`, and `order` is a
// permutation of 0..n-1.
bool legalOrder(const std::vector<uint32_t> &words, const Block &block, const std::vector<size_t> &order)
{
    const size_t n = block.end - block.begin;
    if (order.size() != n || std::set<size_t>(order.begin(), order.end()).size() != n)
    {
        return false;
    }
    std::vector<size_t> position(n);
    for (size_t k = 0; k < n; ++k)
    {
        position[order[k]] = k;
    }
    const DepGraph g = buildDepGraph(words, block, true);
    for (size_t i = 0; i < n; ++i)
    {
        for (size_t j : g.succ[i])
        {
            if (position[i] >= position[j])
            {
                return false;
            }
        }
    }
    return true;
}

// lw then its use, with an independent addi after: rv5-h-f stalls once as written, never when
// the addi moves into the gap.
const char *kLoadUse = R"(.data
v: .word 7
.text
lui x10, 65536
lw x5, 0(x10)
add x6, x5, x5
addi x7, x0, 3
)";

const char *kSumLoop = R"(.data
arr: .word 3, 1, 4, 1, 5, 9
.text
lui x10, 65536
li x6, 6
li x7, 0
loop:
lw x5, 0(x10)
add x7, x7, x5
addi x10, x10, 4
addi x6, x6, -1
bnez x6, loop
li x17, 10
ecall
)";
} // namespace

// ---------------------------------------------------------------------------------------------
// VM safety net: every way a block's new order is refused
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecisionPaths, SafetyNetKeepsEverythingWhenTheOriginalHitsTheCap)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram program = assembleText(kSumLoop);
    const ScheduleReport report = scheduleProgram(program.text_buffer, PipelineKind::HNF, labelLeaders(program),
                                                  true, ScheduleMode::Optimal);
    ASSERT_NE(report.words, program.text_buffer) << "needs a block that would change";
    const SafetyNetResult net = applyWithVmCheck<Kites::RV5StageProcessorHNF>(program, report, {}, 20);
    EXPECT_TRUE(net.originalHitCap);
    EXPECT_EQ(net.words, program.text_buffer);
    EXPECT_TRUE(net.trials.empty());
}

TEST(ReorderDecisionPaths, SafetyNetRejectsAnOrderThatHitsTheCap)
{
    Kites::setupVmStateDirectory();
    using VM = Kites::RV5StageProcessorHF;
    // Already stall-free on rv5-h-f: the addi sits between the load and its use.
    const Kites::AssembledProgram program = assembleText(R"(.data
v: .word 7
.text
lui x10, 65536
lw x5, 0(x10)
addi x7, x0, 3
add x6, x5, x5
)");
    const std::vector<uint32_t> &w = program.text_buffer;
    ASSERT_EQ(w.size(), 4u);
    const std::vector<uint32_t> slower = {w[0], w[2], w[1], w[3]}; // legal, but the load-use pair is adjacent
    const unsigned originalCycles = runOn<VM>(program).cycles;
    const unsigned slowerCycles = runOn<VM>(Kites::withText(program, slower)).cycles;
    ASSERT_GT(slowerCycles, originalCycles);

    // A cap the original fits under and the proposal does not.
    unsigned cap = originalCycles;
    while (cap <= slowerCycles &&
           (runOn<VM>(program, cap).hitCap || !runOn<VM>(Kites::withText(program, slower), cap).hitCap))
    {
        ++cap;
    }
    ASSERT_LE(cap, slowerCycles) << "no cap separates the two runs";

    const SafetyNetResult net = applyWithVmCheck<VM>(program, proposal(program, slower), {}, cap);
    ASSERT_EQ(net.trials.size(), 1u);
    EXPECT_FALSE(net.trials[0].accepted);
    EXPECT_EQ(net.trials[0].reason, SafetyNetTrial::Reason::HitCap);
    EXPECT_STREQ(safetyNetReasonName(net.trials[0].reason), "hit the cycle cap");
    EXPECT_EQ(net.words, program.text_buffer);
    EXPECT_EQ(net.rejectedBlocks(), std::vector<size_t>{net.trials[0].blockIndex});

    // With room to finish, the same proposal is refused for being slower.
    const SafetyNetResult roomy = applyWithVmCheck<VM>(program, proposal(program, slower), {}, kCap);
    ASSERT_EQ(roomy.trials.size(), 1u);
    EXPECT_EQ(roomy.trials[0].reason, SafetyNetTrial::Reason::Slower);
    EXPECT_STREQ(safetyNetReasonName(roomy.trials[0].reason), "slower");
    EXPECT_EQ(roomy.words, program.text_buffer);
    EXPECT_EQ(roomy.finalCycles, roomy.originalCycles);
}

TEST(ReorderDecisionPaths, SafetyNetRejectsAnOrderThatChangesTheState)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram program = assembleText(kLoadUse);
    const std::vector<uint32_t> &w = program.text_buffer;
    const std::vector<uint32_t> broken = {w[0], w[2], w[1], w[3]}; // the add reads x5 before the load
    const SafetyNetResult net =
        applyWithVmCheck<Kites::RV5StageProcessorHF>(program, proposal(program, broken), {}, kCap);
    ASSERT_EQ(net.trials.size(), 1u);
    EXPECT_EQ(net.trials[0].reason, SafetyNetTrial::Reason::DifferentState);
    EXPECT_STREQ(safetyNetReasonName(net.trials[0].reason), "different final state");
    EXPECT_EQ(net.words, program.text_buffer);

    // And a good order is accepted, with its name.
    const std::vector<uint32_t> better = {w[0], w[1], w[3], w[2]};
    const SafetyNetResult ok = applyWithVmCheck<Kites::RV5StageProcessorHF>(program, proposal(program, better), {}, kCap);
    ASSERT_EQ(ok.trials.size(), 1u);
    EXPECT_TRUE(ok.trials[0].accepted);
    EXPECT_STREQ(safetyNetReasonName(ok.trials[0].reason), "accepted");
    EXPECT_LT(ok.finalCycles, ok.originalCycles);
}

// ---------------------------------------------------------------------------------------------
// Optimizer: the node budget may stop the search anywhere; the order must stay legal and never
// be worse than greedy
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecisionPaths, OptimizerStoppedAtAnyBudgetReturnsALegalOrderNoWorseThanGreedy)
{
    Kites::setupVmStateDirectory();
    size_t capped = 0, found = 0;
    for (size_t seed = 0; seed < 6; ++seed)
    {
        // A 16-instruction straight-line block from the structured generator's mix.
        std::mt19937_64 rng(Kites::fuzz::splitMix64(seed + 77));
        Kites::fuzz::structured_detail::Writer w(rng);
        w.line(".text", 0);
        while (w.emitted() < 16)
        {
            w.simple(false);
        }
        const Kites::AssembledProgram program = assembleText(w.text());
        const Block block{0, program.text_buffer.size(), false};
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            const OptimalBlockResult full = scheduleBlockOptimalDetailed(program.text_buffer, block, kind);
            for (uint64_t budget : {1ull, 2ull, 3ull, 5ull, 8ull, 20ull, 50ull, 200ull})
            {
                const OptimalBlockResult r =
                    scheduleBlockOptimalDetailed(program.text_buffer, block, kind, HeadContext{}, true, budget);
                EXPECT_TRUE(legalOrder(program.text_buffer, block, r.order)) << "seed " << seed << " budget " << budget;
                EXPECT_LE(r.optimalStalls, r.greedyStalls);
                EXPECT_GE(r.optimalStalls, full.provenOptimal() ? full.optimalStalls : 0);
                EXPECT_EQ(r.greedyStalls, full.greedyStalls);
                if (!r.provenOptimal())
                {
                    ++capped;
                    found += r.method == OptimizerMethod::BestFound ? 1 : 0;
                    EXPECT_TRUE(r.method == OptimizerMethod::BestFound || r.method == OptimizerMethod::GreedyCapHit);
                    EXPECT_STRNE(optimizerMethodName(r.method), "?");
                }
                // The predicted stalls of the order returned are the ones reported.
                std::vector<DecodedInst> insts;
                for (size_t local : r.order)
                {
                    insts.push_back(decode(program.text_buffer[block.begin + local]));
                }
                EXPECT_EQ(predictStalls(insts, kind), r.optimalStalls);
            }
        }
    }
    EXPECT_GT(capped, 0u) << "a budget this small must stop some searches";
    std::cout << "[ BUDGET ] " << capped << " capped searches, " << found << " of them still beat greedy" << std::endl;
}

TEST(ReorderDecisionPaths, OptimizerSkipsBlocksOverTheSizeLimit)
{
    Kites::setupVmStateDirectory();
    std::ostringstream text;
    text << ".text\nlui x10, 65536\n";
    for (size_t i = 0; i < kMaxOptimalBlockSize; ++i)
    {
        text << (i % 2 == 0 ? "lw x5, 0(x10)\n" : "add x6, x5, x5\n");
    }
    const Kites::AssembledProgram program = assembleText(text.str());
    ASSERT_GT(program.text_buffer.size(), kMaxOptimalBlockSize);
    const Block block{0, program.text_buffer.size(), false};
    const OptimalBlockResult r = scheduleBlockOptimalDetailed(program.text_buffer, block, PipelineKind::HF);
    EXPECT_EQ(r.method, OptimizerMethod::GreedyTooLarge);
    EXPECT_STREQ(optimizerMethodName(r.method), "greedy (block too large to search)");
    EXPECT_FALSE(r.provenOptimal());
    EXPECT_EQ(r.optimalStalls, r.greedyStalls);
    EXPECT_TRUE(legalOrder(program.text_buffer, block, r.order));
}

// ---------------------------------------------------------------------------------------------
// Path replay and unroll prediction
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecisionPaths, BranchConditionsCompareSignedAndUnsigned)
{
    auto branch = [](uint32_t funct3) { return 0x63u | (funct3 << 12); };
    const uint64_t minusOne = ~0ull;
    const uint64_t one = 1;
    EXPECT_FALSE(branchConditionHolds(branch(0b000), minusOne, one));   // beq
    EXPECT_TRUE(branchConditionHolds(branch(0b001), minusOne, one));    // bne
    EXPECT_TRUE(branchConditionHolds(branch(0b100), minusOne, one));    // blt: -1 < 1
    EXPECT_FALSE(branchConditionHolds(branch(0b101), minusOne, one));   // bge
    EXPECT_FALSE(branchConditionHolds(branch(0b110), minusOne, one));   // bltu: 2^64-1 > 1
    EXPECT_TRUE(branchConditionHolds(branch(0b111), minusOne, one));    // bgeu
    EXPECT_TRUE(branchConditionHolds(branch(0b111), one, one));         // bgeu, equal
    EXPECT_FALSE(branchConditionHolds(branch(0b110), one, one));        // bltu, equal
    EXPECT_FALSE(branchConditionHolds(branch(0b010), one, minusOne));   // not a branch funct3
}

TEST(ReorderDecisionPaths, UnsignedBranchesReplayExactly)
{
    Kites::setupVmStateDirectory();
    // x5 = -1 (unsigned: the largest value), x6 = 1; each branch's direction differs between the
    // signed and the unsigned compare.
    const Kites::AssembledProgram program = assembleText(R"(.text
addi x5, x0, -1
addi x6, x0, 1
bltu x5, x6, a
addi x7, x0, 1
a:
bgeu x5, x6, b
addi x7, x0, 2
b:
blt x5, x6, c
addi x8, x0, 3
c:
bge x5, x6, d
addi x8, x0, 4
d:
addi x9, x8, 1
)");
    const ExecutedPath path = tracePath(program);
    ASSERT_TRUE(path.finished);
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const Kites::RunResult vm = kind == PipelineKind::HF ? runOn<Kites::RV5StageProcessorHF>(program)
                                                             : runOn<Kites::RV5StageProcessorHNF>(program);
        const ReplayResult r = replayPath(program, program.text_buffer, path, kind);
        EXPECT_EQ(r.predictedCycles, vm.cycles);
        EXPECT_EQ(r.countedStalls(), vm.stalls);
        EXPECT_EQ(r.takenBranches, 2u); // bgeu and blt
        EXPECT_EQ(vm.regs[7], 1u);      // bltu fell through
        EXPECT_EQ(vm.regs[8], 4u);      // bge fell through
    }
}

TEST(ReorderDecisionPaths, UnrollPredictionCountsJumpsOutsideTheLoop)
{
    Kites::setupVmStateDirectory();
    const std::string source = R"(.text
li x6, 3
loop:
lw x5, 0(x10)
add x7, x7, x5
addi x6, x6, -1
bne x6, x0, loop
jal x0, over
addi x8, x0, 1
over:
add x9, x7, x7
)";
    const Kites::AssembledProgram program = assembleText(source);
    Kites::Seed seed;
    seed.regs.push_back({10, 0x10000000});
    const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
    const std::vector<size_t> loops = eligibleLoops(program.text_buffer, blocks);
    ASSERT_EQ(loops.size(), 1u);
    const ExecutedPath path = tracePath(program, seed);
    ASSERT_TRUE(path.finished);
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const Kites::RunResult vm = kind == PipelineKind::HF ? runOn<Kites::RV5StageProcessorHF>(program, kCap, seed)
                                                             : runOn<Kites::RV5StageProcessorHNF>(program, kCap, seed);
        // k = 1 is the program as it is, jump flush included.
        const UnrollPrediction same = predictUnrolled(program.text_buffer, blocks, path, kind, loops, 1);
        EXPECT_EQ(same.cycles, vm.cycles);
        EXPECT_EQ(same.cycles, replayPath(program, program.text_buffer, path, kind).predictedCycles);

        // k = 2 is the unrolled program's cycles on the VM.
        const UnrolledSource text = unrollSource(source, program, {{blocks[loops[0]], 2}});
        ASSERT_TRUE(text.ok) << text.reason;
        const Kites::AssembledProgram unrolled = assembleText(text.text);
        const Kites::RunResult vmUnrolled = kind == PipelineKind::HF
                                                ? runOn<Kites::RV5StageProcessorHF>(unrolled, kCap, seed)
                                                : runOn<Kites::RV5StageProcessorHNF>(unrolled, kCap, seed);
        EXPECT_EQ(predictUnrolled(program.text_buffer, blocks, path, kind, loops, 2).cycles, vmUnrolled.cycles);
        EXPECT_TRUE(Kites::sameState(vm, vmUnrolled));
    }
}

// ---------------------------------------------------------------------------------------------
// Unrolling: the requests unrollSource() refuses, the shape check, and the verification gate
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecisionPaths, UnrollSourceRefusesBadRequests)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram program = assembleText(kSumLoop);
    const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
    const std::vector<size_t> loops = eligibleLoops(program.text_buffer, blocks);
    ASSERT_EQ(loops.size(), 1u);
    const Block loop = blocks[loops[0]];

    const UnrolledSource once = unrollSource(kSumLoop, program, {{loop, 1}});
    EXPECT_FALSE(once.ok);
    EXPECT_EQ(once.reason, "k must be 2 or more");
    EXPECT_TRUE(once.text.empty());

    const UnrolledSource twice = unrollSource(kSumLoop, program, {{loop, 2}, {loop, 2}});
    EXPECT_FALSE(twice.ok);
    EXPECT_NE(twice.reason.find("is listed twice"), std::string::npos) << twice.reason;

    const UnrolledSource notALoop = unrollSource(kSumLoop, program, {{blocks.front(), 2}});
    EXPECT_FALSE(notALoop.ok);
    EXPECT_NE(notALoop.reason.find("are not an eligible loop"), std::string::npos) << notALoop.reason;
}

TEST(ReorderDecisionPaths, UnrolledShapeCheckRejectsWrongPrograms)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram program = assembleText(kSumLoop);
    const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
    const Block loop = blocks[eligibleLoops(program.text_buffer, blocks)[0]];
    const UnrolledSource text = unrollSource(kSumLoop, program, {{loop, 2}});
    ASSERT_TRUE(text.ok) << text.reason;
    const Kites::AssembledProgram unrolled = assembleText(text.text);
    ASSERT_TRUE(unrolledShapeMatches(program, unrolled, {{loop, 2}}));

    EXPECT_FALSE(unrolledShapeMatches(program, program, {{loop, 2}}));     // nothing added
    EXPECT_FALSE(unrolledShapeMatches(program, unrolled, {{loop, 3}}));    // wrong count

    Kites::AssembledProgram body = unrolled;                               // a copy's body differs
    body.text_buffer[loop.begin + (loop.end - loop.begin)] ^= 1u << 7;
    EXPECT_FALSE(unrolledShapeMatches(program, body, {{loop, 2}}));

    Kites::AssembledProgram notInverted = unrolled;                        // first copy's test not inverted
    const size_t firstTest = loop.end - 1;
    notInverted.text_buffer[firstTest] ^= 1u << 12;
    EXPECT_FALSE(unrolledShapeMatches(program, notInverted, {{loop, 2}}));

    Kites::AssembledProgram notABranch = unrolled;                         // first copy's test is not a branch
    notABranch.text_buffer[firstTest] = 0x00000013;                        // nop
    EXPECT_FALSE(unrolledShapeMatches(program, notABranch, {{loop, 2}}));
}

TEST(ReorderDecisionPaths, UnrollVerificationRefusesEveryKindOfDifference)
{
    Kites::setupVmStateDirectory();
    using VM = Kites::RV5StageProcessorHNF;
    const Kites::AssembledProgram program = assembleText(kSumLoop);
    const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
    const Block loop = blocks[eligibleLoops(program.text_buffer, blocks)[0]];
    const Kites::RunResult original = runOn<VM>(program);
    const Kites::RunResult reorderOnly = reorderWithSafetyNet<VM>(program, PipelineKind::HNF, {}, kCap).run;
    const std::vector<LoopToUnroll> request = {{loop, 2}};

    const UnrollTrial good = tryUnrolling<VM>(kSumLoop, program, request, {}, PipelineKind::HNF, kCap, original,
                                              reorderOnly);
    ASSERT_TRUE(good.ran);
    ASSERT_TRUE(good.passed) << good.reason;

    Kites::RunResult otherState = original;
    otherState.regs[7] ^= 1;
    const UnrollTrial differs = tryUnrolling<VM>(kSumLoop, program, request, {}, PipelineKind::HNF, kCap, otherState,
                                                 reorderOnly);
    EXPECT_FALSE(differs.passed);
    EXPECT_EQ(differs.reason, "its final state differs from the original's");

    Kites::RunResult otherRetired = original;
    otherRetired.retired += 1;
    const UnrollTrial retired = tryUnrolling<VM>(kSumLoop, program, request, {}, PipelineKind::HNF, kCap, otherRetired,
                                                 reorderOnly);
    EXPECT_FALSE(retired.passed);
    EXPECT_EQ(retired.reason, "its retired count differs from the original's");

    Kites::RunResult unbeatable = reorderOnly;
    unbeatable.cycles = 1;
    const UnrollTrial notFaster = tryUnrolling<VM>(kSumLoop, program, request, {}, PipelineKind::HNF, kCap, original,
                                                   unbeatable);
    EXPECT_FALSE(notFaster.passed);
    EXPECT_EQ(notFaster.reason, "it is not faster than reorder-only");

    const UnrollTrial capped = tryUnrolling<VM>(kSumLoop, program, request, {}, PipelineKind::HNF, 10, original,
                                                reorderOnly);
    EXPECT_FALSE(capped.passed);
    EXPECT_EQ(capped.reason, "it hit the cycle cap");

    const UnrollTrial refused = tryUnrolling<VM>(kSumLoop, program, {{loop, 1}}, {}, PipelineKind::HNF, kCap, original,
                                                 reorderOnly);
    EXPECT_FALSE(refused.ran);
    EXPECT_EQ(refused.reason, "k must be 2 or more");
}

TEST(ReorderDecisionPaths, UnrollAndReorderFallsBackToReorderOnly)
{
    Kites::setupVmStateDirectory();
    using VM = Kites::RV5StageProcessorHNF;
    const Kites::AssembledProgram program = assembleText(kSumLoop);

    // The original does not finish under the cap: nothing is unrolled, every loop says why.
    const UnrollOutcome capped = unrollAndReorder<VM>(kSumLoop, program, {}, PipelineKind::HNF, 15);
    EXPECT_FALSE(capped.unrolled);
    ASSERT_EQ(capped.loops.size(), 1u);
    EXPECT_EQ(capped.loops[0].reason, "the original program does not finish cleanly");
    EXPECT_EQ(capped.finalProgram.text_buffer, program.text_buffer);

    // A loop that runs once has nothing to gain (unrolled, it would leave through the inverted
    // test, a taken branch, instead of falling out): not tried.
    const std::string flat = ".text\nli x6, 1\nloop:\naddi x7, x7, 1\naddi x6, x6, -1\nbnez x6, loop\n"
                             "li x17, 10\necall\n";
    const UnrollOutcome noGain =
        unrollAndReorder<Kites::RV5StageProcessorHF>(flat, assembleText(flat), {}, PipelineKind::HF, kCap);
    EXPECT_FALSE(noGain.unrolled);
    ASSERT_EQ(noGain.loops.size(), 1u);
    EXPECT_EQ(noGain.loops[0].k, 1);
    EXPECT_EQ(noGain.loops[0].reason, "no predicted gain");
    EXPECT_FALSE(noGain.loops[0].ran);
}

TEST(ReorderDecisionPaths, PickUnrolledLoopsPrefersTheCombinationThenTheBestSingleLoop)
{
    std::vector<LoopUnrollRecord> records(3);
    records[0].measured = 5;
    records[1].measured = 9;
    records[2].measured = 9;
    EXPECT_EQ(pickUnrolledLoops(records, {0, 1, 2}, true), (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(pickUnrolledLoops(records, {0, 1, 2}, false), std::vector<size_t>{1});   // earlier on a tie
    EXPECT_EQ(pickUnrolledLoops(records, {0}, false), std::vector<size_t>{0});
    EXPECT_TRUE(pickUnrolledLoops(records, {}, false).empty());
}

// ---------------------------------------------------------------------------------------------
// The state comparison also covers the stack window
// ---------------------------------------------------------------------------------------------

TEST(ReorderDecisionPaths, SameStateComparesTheStackWindow)
{
    Kites::setupVmStateDirectory();
    // Both store a different word 1020 bytes past the data start (outside the 64-word data
    // window), then end with the same registers.
    auto program = [](int value)
    {
        return assembleText(".text\nlui x5, 65536\naddi x6, x0, " + std::to_string(value) +
                            "\nsw x6, 1020(x5)\naddi x6, x0, 0\n");
    };
    const uint64_t stackTop = 0x10000000ull + 1024;
    using VM = Kites::RV5StageProcessorHF;
    const Kites::RunResult a = Kites::runProgram<VM>(program(7), {}, kCap, 64, stackTop, 4);
    const Kites::RunResult b = Kites::runProgram<VM>(program(8), {}, kCap, 64, stackTop, 4);
    ASSERT_EQ(a.stackWords.size(), 4u);
    EXPECT_EQ(a.stackWords[0], 7u);
    EXPECT_EQ(a.regs, b.regs);
    EXPECT_EQ(a.dataWords, b.dataWords);
    EXPECT_FALSE(Kites::sameState(a, b));
    EXPECT_NE(Kites::fuzz::describeDifference(a, b).find("stack word 0"), std::string::npos);

    const Kites::RunResult noStackA = Kites::runProgram<VM>(program(7), {}, kCap);
    const Kites::RunResult noStackB = Kites::runProgram<VM>(program(8), {}, kCap);
    EXPECT_TRUE(Kites::sameState(noStackA, noStackB)); // without a stack window the difference is not seen
}
