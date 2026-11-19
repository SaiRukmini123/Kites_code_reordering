// Tests for src/reorder/path_replay.h: one small program per control rule,
// the per-stall records, the tracer's exit-ecall stop, and a model-validation
// run over every benchmark in bench/programs on both pipelines.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/bench_eval.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder/vm_safety_net.h"
#include "utils/utils.h"

using namespace Kites::reorder;

namespace
{

Kites::AssembledProgram assembleText(const std::string &source)
{
    std::istringstream s(source);
    return Kites::assemble(s);
}

struct ReplayAndVm
{
    ReplayResult replay;
    Kites::RunResult vm;
};

// Traces `program` on the single-cycle VM, replays it unchanged for one
// pipeline, and runs it on that pipeline's VM.
template <class VM>
ReplayAndVm replayAndRun(const Kites::AssembledProgram &program, PipelineKind kind)
{
    const ExecutedPath path = tracePath(program);
    EXPECT_TRUE(path.finished);
    ReplayAndVm r;
    r.replay = replayPath(program, program.text_buffer, path, kind);
    r.vm = Kites::runProgram<VM>(program, {}, 10000);
    return r;
}

// The replay must give the VM's cycle count and its stall counter exactly.
void expectSameAsVm(const ReplayAndVm &r, const std::string &what)
{
    EXPECT_FALSE(r.vm.hitCap) << what;
    EXPECT_EQ(r.replay.predictedCycles, r.vm.cycles) << what;
    EXPECT_EQ(r.replay.countedStalls(), r.vm.stalls) << what;
    EXPECT_EQ(r.replay.retired, r.vm.retired) << what;
}

// The record of the hazard whose consumer sits at `consumerPc`, or nullptr.
const StallRecord *recordFor(const ReplayResult &r, uint64_t consumerPc)
{
    for (const StallRecord &s : r.stalls)
    {
        if (s.consumerPc == consumerPc)
        {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

// Falling into the next block (a label with no branch, then a branch that is
// not taken) keeps the hazard window, so the hazard crosses the boundary.
TEST(ReorderPathReplay, FallThroughKeepsTheWindow)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram p = assembleText(R"(.data
w: .word 7
.text
    lui x10, 65536          # 0: x10 = start of .data
    addi x0, x0, 0          # 4
    addi x0, x0, 0          # 8
    lw x5, 0(x10)           # 12
next:
    add x6, x5, x5          # 16: new block, entered by falling through; waits for the load
    addi x7, x0, 1          # 20
    bne x0, x0, next        # 24: never taken
    add x28, x7, x7         # 28: new block; x7 is two instructions back, across the branch
    addi x17, x0, 10        # 32
    ecall                   # 36
)");
    const ReplayAndVm f = replayAndRun<Kites::RV5StageProcessorHF>(p, PipelineKind::HF);
    const ReplayAndVm n = replayAndRun<Kites::RV5StageProcessorHNF>(p, PipelineKind::HNF);
    expectSameAsVm(f, "rv5-h-f");
    expectSameAsVm(n, "rv5-h-nf");

    // rv5-h-f: only the load-use across the label.
    EXPECT_EQ(f.replay.stallCycles, 1u);
    ASSERT_EQ(f.replay.stalls.size(), 1u);
    EXPECT_EQ(f.replay.stalls[0].consumerPc, 16u);
    EXPECT_EQ(f.replay.stalls[0].producerPc, 12u);
    EXPECT_TRUE(f.replay.stalls[0].producerInEarlierBlock);

    // rv5-h-nf: 2 cycles across the label, 1 across the not-taken branch.
    EXPECT_EQ(n.replay.stallCycles, 3u);
    const StallRecord *label = recordFor(n.replay, 16);
    const StallRecord *branch = recordFor(n.replay, 28);
    ASSERT_NE(label, nullptr);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(label->producerPc, 12u);
    EXPECT_EQ(label->cycles, 2);
    EXPECT_EQ(branch->producerPc, 20u);
    EXPECT_EQ(branch->cycles, 1);
    for (const StallRecord *s : {label, branch})
    {
        EXPECT_TRUE(s->producerInEarlierBlock);
        EXPECT_TRUE(s->trueRead);
        EXPECT_FALSE(s->discarded);
    }
    EXPECT_EQ(n.replay.flushCycles, 0u);
}

// A taken branch costs 3 cycles and its target starts with an empty window.
// The second program branches to the very next instruction: still a flush,
// which is why the tracer evaluates the condition instead of looking at the
// next PC.
TEST(ReorderPathReplay, TakenBranchClearsTheWindow)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram p = assembleText(R"(
    addi x5, x0, 1          # 0
    beq x0, x0, target      # 4: taken
    addi x0, x0, 0          # 8: fetched behind the branch, discarded
target:
    add x6, x5, x5          # 12: x5 is two instructions back, but the window was cleared
    addi x17, x0, 10        # 16
    ecall                   # 20
)");
    for (const auto kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const ReplayAndVm r = kind == PipelineKind::HF ? replayAndRun<Kites::RV5StageProcessorHF>(p, kind)
                                                       : replayAndRun<Kites::RV5StageProcessorHNF>(p, kind);
        expectSameAsVm(r, "taken branch");
        EXPECT_EQ(r.replay.takenBranches, 1u);
        EXPECT_EQ(r.replay.flushCycles, 3u);
        EXPECT_EQ(r.replay.stallCycles, 0u);
        EXPECT_EQ(r.replay.predictedCycles, 5u + 4u + 3u);
    }

    const Kites::AssembledProgram toNext = assembleText(R"(
    addi x5, x0, 1          # 0
    beq x0, x0, next        # 4: taken, to the next instruction
next:
    add x6, x5, x5          # 8: checked once behind the branch (discarded), then fetched again clean
    addi x17, x0, 10        # 12
    ecall                   # 16
)");
    const ExecutedPath path = tracePath(toNext);
    ASSERT_EQ(path.indices.size(), 5u);
    EXPECT_TRUE(path.taken[1]);
    const ReplayAndVm f = replayAndRun<Kites::RV5StageProcessorHF>(toNext, PipelineKind::HF);
    const ReplayAndVm n = replayAndRun<Kites::RV5StageProcessorHNF>(toNext, PipelineKind::HNF);
    expectSameAsVm(f, "branch to next, rv5-h-f");
    expectSameAsVm(n, "branch to next, rv5-h-nf");
    EXPECT_EQ(n.replay.flushCycles, 3u);
    EXPECT_EQ(n.replay.stallCycles, 0u);
    EXPECT_EQ(n.replay.discardedStalls, 1u);
}

// A jump (jal or jalr) costs 2 cycles and clears the window; the instruction
// behind it is discarded before the hazard check, so nothing is counted for it
// even though it would wait for x10 on rv5-h-nf.
TEST(ReorderPathReplay, JumpClearsTheWindow)
{
    Kites::setupVmStateDirectory();
    const char *programs[] = {R"(
    addi x10, x0, 4         # 0
    j target                # 4
    add x6, x10, x10        # 8: discarded, never hazard-checked
target:
    add x7, x10, x10        # 12: x10 is two instructions back, but the window was cleared
    addi x17, x0, 10        # 16
    ecall                   # 20
)",
                              R"(
    addi x10, x0, 4         # 0
    jalr x0, 12(x0)         # 4: to address 12
    add x6, x10, x10        # 8: discarded, never hazard-checked
    add x7, x10, x10        # 12
    addi x17, x0, 10        # 16
    ecall                   # 20
)"};
    for (const char *source : programs)
    {
        const Kites::AssembledProgram p = assembleText(source);
        for (const auto kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            const ReplayAndVm r = kind == PipelineKind::HF ? replayAndRun<Kites::RV5StageProcessorHF>(p, kind)
                                                           : replayAndRun<Kites::RV5StageProcessorHNF>(p, kind);
            expectSameAsVm(r, source);
            EXPECT_EQ(r.replay.jumps, 1u);
            EXPECT_EQ(r.replay.flushCycles, 2u);
            EXPECT_EQ(r.replay.countedStalls(), 0u);
            EXPECT_EQ(r.replay.predictedCycles, 5u + 4u + 2u);
        }
    }
}

// The instruction fetched behind a taken branch is hazard-checked before the
// flush discards it: rv5-h-nf counts its stall in total_stalls_, but no cycle
// is lost. Here its immediate 10 sits in the rs2 field and reads as x10 (the
// HDU quirk); the second program has a true read instead.
TEST(ReorderPathReplay, DiscardedInstructionStallIsCountedButFree)
{
    Kites::setupVmStateDirectory();
    const std::pair<const char *, bool> programs[] = {{R"(
    addi x10, x0, 4         # 0: written right before the taken branch
    beq x0, x0, target      # 4
    addi x17, x0, 10        # 8: discarded; immediate 10 = rs2 field x10
target:
    addi x6, x0, 1          # 12
    addi x17, x0, 10        # 16
    ecall                   # 20
)",
                                                       false},
                                                      {R"(
    addi x10, x0, 4         # 0
    beq x0, x0, target      # 4
    add x6, x10, x10        # 8: discarded; really reads x10
target:
    addi x6, x0, 1          # 12
    addi x17, x0, 10        # 16
    ecall                   # 20
)",
                                                       true}};
    for (const auto &[source, trueRead] : programs)
    {
        const Kites::AssembledProgram p = assembleText(source);
        const ReplayAndVm f = replayAndRun<Kites::RV5StageProcessorHF>(p, PipelineKind::HF);
        const ReplayAndVm n = replayAndRun<Kites::RV5StageProcessorHNF>(p, PipelineKind::HNF);
        expectSameAsVm(f, source);
        expectSameAsVm(n, source);
        EXPECT_EQ(f.replay.countedStalls(), 0u);   // rv5-h-f: the branch is not a load

        EXPECT_EQ(n.replay.stallCycles, 0u);
        EXPECT_EQ(n.replay.discardedStalls, 1u);
        EXPECT_EQ(n.vm.stalls, 1u);
        EXPECT_EQ(n.vm.cycles, 5u + 4u + 3u);   // the counted stall costs no cycle
        const StallRecord *s = recordFor(n.replay, 8);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->producerPc, 0u);
        EXPECT_EQ(s->cycles, 1);
        EXPECT_TRUE(s->discarded);
        EXPECT_TRUE(s->producerInEarlierBlock);
        EXPECT_EQ(s->trueRead, trueRead);
    }
}

// Inside one block: a load-use with a true read, and an immediate whose bits
// name the register just written (addi x17, x0, 10 reads as x10).
TEST(ReorderPathReplay, StallRecordsTellTrueReadsFromImmediateBits)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram p = assembleText(R"(.data
w: .word 3
.text
    lui x10, 65536          # 0
    addi x0, x0, 0          # 4
    addi x0, x0, 0          # 8
    lw x5, 0(x10)           # 12
    addi x6, x5, 1          # 16: load-use, true read
    addi x10, x0, 5         # 20
    addi x17, x0, 10        # 24: immediate 10 = rs2 field x10, not a read
    ecall                   # 28
)");
    const ReplayAndVm f = replayAndRun<Kites::RV5StageProcessorHF>(p, PipelineKind::HF);
    const ReplayAndVm n = replayAndRun<Kites::RV5StageProcessorHNF>(p, PipelineKind::HNF);
    expectSameAsVm(f, "rv5-h-f");
    expectSameAsVm(n, "rv5-h-nf");

    ASSERT_EQ(f.replay.stalls.size(), 1u);
    EXPECT_EQ(f.replay.stalls[0].consumerPc, 16u);
    EXPECT_TRUE(f.replay.stalls[0].trueRead);
    EXPECT_FALSE(f.replay.stalls[0].producerInEarlierBlock);

    const StallRecord *load = recordFor(n.replay, 16);
    const StallRecord *imm = recordFor(n.replay, 24);
    ASSERT_NE(load, nullptr);
    ASSERT_NE(imm, nullptr);
    EXPECT_TRUE(load->trueRead);
    EXPECT_EQ(imm->producerPc, 20u);
    EXPECT_EQ(imm->cycles, 2);
    EXPECT_FALSE(imm->trueRead);
    EXPECT_FALSE(imm->producerInEarlierBlock);
    EXPECT_FALSE(imm->discarded);
}

// The tracer stops before the exit ecall (the single-cycle VM would call
// exit(0) and end this test process), runs other syscalls, and also finishes
// when a program runs off the end of its text.
TEST(ReorderPathReplay, TracerStopsBeforeTheExitEcall)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram withPrint = assembleText(R"(
    addi x10, x0, 42        # 0
    addi x17, x0, 1         # 4: print-int syscall, not exit
    ecall                   # 8
    addi x17, x0, 10        # 12
    ecall                   # 16: exit
)");
    const ExecutedPath a = tracePath(withPrint);
    EXPECT_TRUE(a.finished);
    EXPECT_TRUE(a.endedAtExitEcall);
    EXPECT_EQ(a.indices, (std::vector<uint32_t>{0, 1, 2, 3, 4}));
    const ReplayAndVm n = replayAndRun<Kites::RV5StageProcessorHNF>(withPrint, PipelineKind::HNF);
    expectSameAsVm(n, "print then exit");

    const Kites::AssembledProgram noEcall = assembleText(R"(
    addi x5, x0, 2          # 0
loop:
    addi x5, x5, -1         # 4
    bne x5, x0, loop        # 8
    addi x6, x0, 1          # 12
)");
    const ExecutedPath b = tracePath(noEcall);
    EXPECT_TRUE(b.finished);
    EXPECT_FALSE(b.endedAtExitEcall);
    EXPECT_EQ(b.indices, (std::vector<uint32_t>{0, 1, 2, 1, 2, 3}));
    EXPECT_EQ(b.taken, (std::vector<bool>{false, false, true, false, false, false}));
    const ReplayAndVm f = replayAndRun<Kites::RV5StageProcessorHF>(noEcall, PipelineKind::HF);
    expectSameAsVm(f, "no ecall");
}

namespace
{

struct BenchCounts
{
    int programs = 0;
    int checks = 0;
};

// One benchmark on one pipeline: the original, and the Optimal schedule after
// the VM safety net, each replayed from the same trace and run on the VM.
template <class VM>
void expectBenchmarkMatches(const Kites::AssembledProgram &program, const Kites::Seed &seed,
                            const ExecutedPath &path, const std::vector<Block> &blocks, PipelineKind kind,
                            const std::string &name, BenchCounts &counts)
{
    const unsigned cap = Kites::bench_eval::kBenchCycleCap;
    const std::string where = name + (kind == PipelineKind::HF ? " on rv5-h-f" : " on rv5-h-nf");

    const Kites::RunResult before = Kites::runProgram<VM>(program, seed, cap);
    const ReplayResult rb = replayPath(program.text_buffer, blocks, path, kind);
    EXPECT_EQ(rb.predictedCycles, before.cycles) << where << ", original";
    EXPECT_EQ(rb.countedStalls(), before.stalls) << where << ", original";

    const ScheduleReport report =
        scheduleProgram(program.text_buffer, kind, labelLeaders(program), true, ScheduleMode::Optimal);
    const SafetyNetResult net = applyWithVmCheck<VM>(program, report, seed, cap);
    const Kites::RunResult after = Kites::runProgram<VM>(Kites::withText(program, net.words), seed, cap);
    const ReplayResult ra = replayPath(net.words, blocks, path, kind);
    EXPECT_EQ(ra.predictedCycles, after.cycles) << where << ", Optimal + safety net";
    EXPECT_EQ(ra.countedStalls(), after.stalls) << where << ", Optimal + safety net";
    counts.checks += 2;
}

} // namespace

// Model validation: for every benchmark the bench filter accepts, on both
// pipelines, before and after Optimal + safety net, the replay gives the VM's
// cycle count and its total_stalls_ exactly.
TEST(ReorderPathReplay, MatchesTheVmOnEveryBenchmark)
{
    namespace be = Kites::bench_eval;
    Kites::setupVmStateDirectory();
    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::filesystem::path> paths = be::findPrograms("../bench/programs");
    ASSERT_FALSE(paths.empty());

    BenchCounts counts;
    std::vector<std::string> skipped;
    for (const auto &path : paths)
    {
        const std::string source = be::readFile(path);
        const be::ProgramHeader header = be::parseProgramHeader(source);
        const std::string name = header.has("name") ? header.get("name") : path.stem().string();
        if (!be::assemblerHangLabel(source).empty())
        {
            skipped.push_back(name + " (the assembler would hang on it)");
            continue;
        }
        const Kites::AssembledProgram program = Kites::assemble(path.string());
        Kites::Seed seed;
        std::vector<std::string> problems = be::staticValidityProblems(program, header, seed);
        if (problems.empty())
        {
            problems = be::dynamicValidityProblems(
                Kites::runProgram<Kites::RV5StageProcessorHF>(program, seed, be::kBenchCycleCap),
                Kites::runProgram<Kites::RV5StageProcessorHNF>(program, seed, be::kBenchCycleCap));
        }
        if (!problems.empty())
        {
            skipped.push_back(name + " (" + problems.front() + ")");
            continue;
        }

        const ExecutedPath trace = tracePath(program, seed, 10ull * be::kBenchCycleCap);
        ASSERT_TRUE(trace.finished) << name;
        // The tracer's branch outcomes agree with where the VM went next.
        for (size_t t = 0; t + 1 < trace.indices.size(); ++t)
        {
            const uint32_t word = program.text_buffer[trace.indices[t]];
            if (decode(word).is_branch && branchImm(word) != 4)
            {
                EXPECT_EQ(trace.taken[t], trace.indices[t + 1] != trace.indices[t] + 1) << name << " step " << t;
            }
        }
        const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
        expectBenchmarkMatches<Kites::RV5StageProcessorHF>(program, seed, trace, blocks, PipelineKind::HF, name,
                                                           counts);
        expectBenchmarkMatches<Kites::RV5StageProcessorHNF>(program, seed, trace, blocks, PipelineKind::HNF, name,
                                                            counts);
        ++counts.programs;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[ REPLAY ] " << counts.programs << " benchmarks, " << counts.checks
              << " program runs checked (2 pipelines x original and Optimal + safety net) in " << seconds
              << " s; skipped " << skipped.size();
    for (const std::string &s : skipped)
    {
        std::cout << " | " << s;
    }
    std::cout << std::endl;
    EXPECT_GT(counts.programs, 0);
}
