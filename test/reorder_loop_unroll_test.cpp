// Tests for opt-in loop unrolling: src/reorder/unroll_predict.h (predict) and
// src/reorder/loop_unroll.h (transform and verify).

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/bench_eval.h"
#include "reorder/fuzz.h"
#include "reorder/loop_unroll.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/unroll_predict.h"
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
Kites::RunResult runOn(const Kites::AssembledProgram &program, const Kites::Seed &seed = {})
{
    return Kites::runProgram<VM>(program, seed, kCap, Kites::reorder_report::kDataWindowWords);
}

std::vector<Block> blocksOf(const Kites::AssembledProgram &program)
{
    return findBlocks(program.text_buffer, labelLeaders(program));
}

// The program's only eligible loop.
Block onlyLoop(const Kites::AssembledProgram &program)
{
    const std::vector<Block> blocks = blocksOf(program);
    const std::vector<size_t> loops = eligibleLoops(program.text_buffer, blocks);
    EXPECT_EQ(loops.size(), 1u);
    return loops.empty() ? Block{0, 0, false} : blocks[loops.front()];
}

// How many lines carry the label `name`.
int labelCount(const std::string &source, const std::string &name)
{
    int n = 0;
    for (const std::string &line : splitLines(source))
    {
        for (const std::string &label : unroll_detail::parseLine(line).labels)
        {
            n += label == name ? 1 : 0;
        }
    }
    return n;
}

// How many lines have exactly this instruction text (labels and comment removed).
int codeCount(const std::string &source, const std::string &code)
{
    int n = 0;
    for (const std::string &line : splitLines(source))
    {
        n += unroll_detail::parseLine(line).code == code ? 1 : 0;
    }
    return n;
}

const char *kName[] = {"rv5-h-f", "rv5-h-nf"};

// Unrolls the program's only loop k times, without reordering, and checks it
// against the original on one pipeline:
//  - same final state and retired count;
//  - the estimator (original order in every copy) predicts its cycles exactly;
//  - the replay of its own trace gives its cycles and total_stalls_ exactly.
template <class VM>
void expectUnrolledMatches(const std::string &source, int k, PipelineKind kind, const std::string &what)
{
    const Kites::AssembledProgram program = assembleText(source);
    const Block loop = onlyLoop(program);
    const UnrolledSource text = unrollSource(source, program, {{loop, k}});
    ASSERT_TRUE(text.ok) << what << ": " << text.reason;
    const Kites::AssembledProgram unrolled = assembleText(text.text);
    ASSERT_TRUE(unrolledShapeMatches(program, unrolled, {{loop, k}})) << what;

    const Kites::RunResult before = runOn<VM>(program);
    const Kites::RunResult after = runOn<VM>(unrolled);
    ASSERT_FALSE(before.hitCap) << what;
    EXPECT_TRUE(Kites::sameState(before, after)) << what;
    EXPECT_EQ(after.retired, before.retired) << what;

    const ExecutedPath path = tracePath(program);
    const std::vector<Block> blocks = blocksOf(program);
    size_t loopIndex = 0;
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        loopIndex = blocks[b].begin == loop.begin ? b : loopIndex;
    }
    EXPECT_EQ(predictUnrolled(program.text_buffer, blocks, path, kind, {loopIndex}, k).cycles, after.cycles) << what;

    const ExecutedPath own = tracePath(unrolled);
    const ReplayResult replay = replayPath(unrolled, unrolled.text_buffer, own, kind);
    EXPECT_EQ(replay.predictedCycles, after.cycles) << what;
    EXPECT_EQ(replay.countedStalls(), after.stalls) << what;
}

} // namespace

// ---------------------------------------------------------------------------
// The inversion table
// ---------------------------------------------------------------------------

TEST(ReorderLoopUnroll, InversionTable)
{
    const std::pair<const char *, const char *> pairs[] = {
        {"beq", "bne"},   {"blt", "bge"},   {"bltu", "bgeu"}, {"bgt", "ble"},
        {"bgtu", "bleu"}, {"beqz", "bnez"}, {"bltz", "bgez"}, {"bgtz", "blez"},
    };
    for (const auto &[a, b] : pairs)
    {
        EXPECT_EQ(invertedBranch(a), b);
        EXPECT_EQ(invertedBranch(b), a);
        const int operands = std::string(a).back() == 'z' ? 2 : 3;
        EXPECT_EQ(branchOperandCount(a), operands) << a;
        EXPECT_EQ(branchOperandCount(b), operands) << b;
    }
    for (const char *other : {"j", "jal", "jalr", "call", "ret", "add", "beqzz", ""})
    {
        EXPECT_EQ(invertedBranch(other), "") << other;
        EXPECT_EQ(branchOperandCount(other), 0) << other;
    }
}

// Every branch and its inverse, on the VM: for the same operands exactly one
// of the two jumps (signed and unsigned comparisons included).
TEST(ReorderLoopUnroll, InvertedBranchesTakeTheOtherWay)
{
    Kites::setupVmStateDirectory();
    const char *mnemonics[] = {"beq",  "bne",  "blt",  "bge",  "bltu", "bgeu", "bgt",  "ble",
                               "bgtu", "bleu", "beqz", "bnez", "bltz", "bgez", "bgtz", "blez"};
    const int values[] = {-2, -1, 0, 1, 2};
    auto jumps = [](const std::string &mnemonic, int a, int b)
    {
        const bool zeroForm = mnemonic.back() == 'z';
        std::ostringstream src;
        src << "li x5, " << a << "\nli x6, " << b << "\n"
            << mnemonic << " x5, " << (zeroForm ? "" : "x6, ") << "yes\n"
            << "li x10, 0\nj done\nyes:\nli x10, 1\ndone:\nli x17, 10\necall\n";
        return runOn<Kites::RV5StageProcessorHNF>(assembleText(src.str())).regs[10];
    };
    for (const char *m : mnemonics)
    {
        const std::string inverse = invertedBranch(m);
        for (int a : values)
        {
            for (int b : values)
            {
                EXPECT_NE(jumps(m, a, b), jumps(inverse, a, b)) << m << " / " << inverse << " with " << a << ", " << b;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Labels, comments, and the generated exit label
// ---------------------------------------------------------------------------

const char *kLabelOnLine = R"(.data
arr: .word 1, 2, 3, 4, 5, 6, 7, 8
.text
    lui x10, 65536          # x10 = arr
    li x6, 8                # trip count
    li x7, 0
loop: lw x5, 0(x10)   # load
    addi x10, x10, 4  # bump
    add x7, x7, x5
    addi x6, x6, -1
    bnez x6, loop     # back
    li x17, 10
    ecall
)";

TEST(ReorderLoopUnroll, LabelOnTheInstructionLineStaysOnlyInTheFirstCopy)
{
    Kites::setupVmStateDirectory();
    const Kites::AssembledProgram program = assembleText(kLabelOnLine);
    const Block loop = onlyLoop(program);
    const unsigned branchLine = sourceLineOf(program, loop.end - 1);
    for (int k : {2, 4})
    {
        const UnrolledSource text = unrollSource(kLabelOnLine, program, {{loop, k}});
        ASSERT_TRUE(text.ok) << text.reason;
        const std::string exit = "unroll_exit_L" + std::to_string(branchLine);
        ASSERT_EQ(text.exitLabels, std::vector<std::string>{exit});
        EXPECT_EQ(labelCount(text.text, "loop"), 1);
        EXPECT_EQ(labelCount(text.text, exit), 1);
        EXPECT_EQ(codeCount(text.text, "lw x5, 0(x10)"), k);
        EXPECT_EQ(codeCount(text.text, "beqz x6, " + exit), k - 1);
        EXPECT_EQ(codeCount(text.text, "bnez x6, loop"), 1);
        int bumps = 0;
        for (size_t at = text.text.find("# bump"); at != std::string::npos; at = text.text.find("# bump", at + 1))
        {
            ++bumps;
        }
        EXPECT_EQ(bumps, k);   // comments are copied with their lines

        // The real assembler takes the generated name, at the instruction after the loop.
        const Kites::AssembledProgram unrolled = assembleText(text.text);
        ASSERT_EQ(unrolled.symbol_table.count(exit), 1u);
        const size_t length = loop.end - loop.begin;
        EXPECT_EQ(unrolled.symbol_table.at(exit).address, 4ull * (loop.begin + static_cast<size_t>(k) * length));
        EXPECT_TRUE(unrolledShapeMatches(program, unrolled, {{loop, k}}));
        expectUnrolledMatches<Kites::RV5StageProcessorHNF>(kLabelOnLine, k, PipelineKind::HNF, "label on line");
    }
}

TEST(ReorderLoopUnroll, LabelsOnTheirOwnLineAndSeveralLabels)
{
    Kites::setupVmStateDirectory();
    const std::string source = R"(
    li x5, 6
start:
again: top: addi x5, x5, -1   # two labels on the first line, one above it
    addi x7, x7, 3
    bnez x5, again
    li x17, 10
    ecall
)";
    const Kites::AssembledProgram program = assembleText(source);
    const Block loop = onlyLoop(program);
    const UnrolledSource text = unrollSource(source, program, {{loop, 4}});
    ASSERT_TRUE(text.ok) << text.reason;
    for (const char *label : {"start", "again", "top"})
    {
        EXPECT_EQ(labelCount(text.text, label), 1) << label;
    }
    EXPECT_EQ(codeCount(text.text, "addi x5, x5, -1"), 4);
    EXPECT_EQ(codeCount(text.text, "beqz x5, " + text.exitLabels.front()), 3);
    for (int k : {2, 4})
    {
        expectUnrolledMatches<Kites::RV5StageProcessorHF>(source, k, PipelineKind::HF, "labels, rv5-h-f");
        expectUnrolledMatches<Kites::RV5StageProcessorHNF>(source, k, PipelineKind::HNF, "labels, rv5-h-nf");
    }
}

TEST(ReorderLoopUnroll, ExitLabelAvoidsExistingNames)
{
    Kites::setupVmStateDirectory();
    // The loop's back-branch is on line 5, so the first-choice name is
    // unroll_exit_L5: here it is already a data label and a code label.
    const std::string source = R"(.data
unroll_exit_L5: .word 7
.text
loop: addi x5, x5, 1
    blt x5, x6, loop
unroll_exit_L5_:
    li x17, 10
    ecall
)";
    const Kites::AssembledProgram program = assembleText(source);
    const Block loop = onlyLoop(program);
    ASSERT_EQ(sourceLineOf(program, loop.end - 1), 5u);
    const UnrolledSource text = unrollSource(source, program, {{loop, 2}});
    ASSERT_TRUE(text.ok) << text.reason;
    EXPECT_EQ(text.exitLabels.front(), "unroll_exit_L5__");
    const Kites::AssembledProgram unrolled = assembleText(text.text);
    EXPECT_EQ(unrolled.symbol_table.count("unroll_exit_L5__"), 1u);
    EXPECT_TRUE(unrolledShapeMatches(program, unrolled, {{loop, 2}}));
    // Generated names never look like a section name (the assembler hangs on those).
    for (unsigned line : {1u, 5u, 99u})
    {
        std::set<std::string> taken;
        EXPECT_EQ(Kites::bench_eval::assemblerHangLabel(exitLabelName(line, taken) + ":\n"), "");
    }
}

TEST(ReorderLoopUnroll, TwoLoopsGetTheirOwnExitLabels)
{
    Kites::setupVmStateDirectory();
    const std::string source = R"(
    li x5, 5
first:
    addi x7, x7, 2
    addi x5, x5, -1
    bgtz x5, first
    li x6, 3
second:
    addi x8, x8, 5
    addi x6, x6, -1
    bnez x6, second
    li x17, 10
    ecall
)";
    const Kites::AssembledProgram program = assembleText(source);
    const std::vector<Block> blocks = blocksOf(program);
    const std::vector<size_t> loops = eligibleLoops(program.text_buffer, blocks);
    ASSERT_EQ(loops.size(), 2u);
    const std::vector<LoopToUnroll> both = {{blocks[loops[0]], 2}, {blocks[loops[1]], 4}};
    const UnrolledSource text = unrollSource(source, program, both);
    ASSERT_TRUE(text.ok) << text.reason;
    ASSERT_EQ(text.exitLabels.size(), 2u);
    EXPECT_NE(text.exitLabels[0], text.exitLabels[1]);
    const Kites::AssembledProgram unrolled = assembleText(text.text);
    EXPECT_TRUE(unrolledShapeMatches(program, unrolled, both));
    const Kites::RunResult before = runOn<Kites::RV5StageProcessorHNF>(program);
    const Kites::RunResult after = runOn<Kites::RV5StageProcessorHNF>(unrolled);
    EXPECT_TRUE(Kites::sameState(before, after));
    EXPECT_EQ(after.retired, before.retired);
}

// ---------------------------------------------------------------------------
// Exclusions
// ---------------------------------------------------------------------------

TEST(ReorderLoopUnroll, LoopsThatAreNotEligible)
{
    Kites::setupVmStateDirectory();
    const std::pair<const char *, const char *> cases[] = {
        {"li x6, 3\nloop:\nla x10, val\nlw x5, 0(x10)\naddi x6, x6, -1\nbnez x6, loop\n"
         "li x17, 10\necall\n.data\nval: .word 4\n",
         "it contains an auipc (la)"},
        // A call, written as jal ra: the Kites assembler does not implement the call pseudo.
        {"j main\nfn:\naddi x7, x7, 1\nret\nmain:\nli x6, 3\nloop:\njal ra, fn\naddi x6, x6, -1\nbnez x6, loop\n"
         "li x17, 10\necall\n",
         "it contains a call, j, ret or ecall"},
        {"li x6, 3\nloop:\naddi x7, x7, 1\nj skip\nskip:\naddi x6, x6, -1\nbnez x6, loop\nli x17, 10\necall\n",
         "it contains a call, j, ret or ecall"},
        {"li x6, 2\nloop:\naddi x6, x6, -1\nret\nbnez x6, loop\nli x17, 10\necall\n",
         "it contains a call, j, ret or ecall"},
        {"li x6, 2\nloop:\naddi x17, x0, 1\necall\naddi x6, x6, -1\nbnez x6, loop\nli x17, 10\necall\n",
         "it contains a call, j, ret or ecall"},
        {"li x6, 4\nloop:\nandi x7, x6, 1\nbeqz x7, even\naddi x8, x8, 1\neven:\naddi x6, x6, -1\nbnez x6, loop\n"
         "li x17, 10\necall\n",
         "it has more than one basic block"},
        {"li x6, 3\nloop:\nbeqz x6, done\naddi x6, x6, -1\nj loop\ndone:\nli x17, 10\necall\n",
         "it jumps back with j"},
    };
    for (const auto &[source, reason] : cases)
    {
        Kites::AssembledProgram program;
        try
        {
            program = assembleText(source);
        }
        catch (const std::exception &e)
        {
            ADD_FAILURE() << "does not assemble (" << e.what() << "):\n" << source;
            continue;
        }
        const std::vector<Block> blocks = blocksOf(program);
        EXPECT_TRUE(eligibleLoops(program.text_buffer, blocks).empty()) << source;
        const std::vector<IneligibleLoop> other = ineligibleLoops(program, blocks);
        ASSERT_EQ(other.size(), 1u) << source;
        EXPECT_EQ(other.front().reason, reason) << source;
    }
}

// An eligible loop that cannot be unrolled on the text: unrolling moves code,
// and only labels follow it.
TEST(ReorderLoopUnroll, TextThatCannotBeUnrolledSafely)
{
    Kites::setupVmStateDirectory();
    const std::pair<std::string, std::string> cases[] = {
        {"li x6, 3\nloop:\naddi x7, x7, 1\naddi x6, x6, -1\nbnez x6, loop\nauipc x10, 0\nli x17, 10\necall\n",
         "hand-written auipc"},
        {"li x6, 3\nloop:\naddi x7, x7, 1\naddi x6, x6, -1\nbnez x6, loop\nli x17, 10\necall\n.data\nloop2: .word 1\n",
         ""},
    };
    for (const auto &[source, reasonPart] : cases)
    {
        const Kites::AssembledProgram program = assembleText(source);
        const UnrolledSource text = unrollSource(source, program, {{onlyLoop(program), 2}});
        if (reasonPart.empty())
        {
            EXPECT_TRUE(text.ok) << text.reason;   // a data label does not get in the way
        }
        else
        {
            EXPECT_FALSE(text.ok);
            EXPECT_NE(text.reason.find(reasonPart), std::string::npos) << text.reason;
        }
    }

    // A branch to a numeric offset, if the assembler takes one at all.
    const std::string numeric =
        "li x6, 3\nloop:\naddi x7, x7, 1\naddi x6, x6, -1\nbnez x6, loop\nbeq x0, x0, 8\nli x17, 10\necall\n";
    Kites::AssembledProgram program;
    try
    {
        program = assembleText(numeric);
    }
    catch (const std::exception &)
    {
        return;   // not accepted by the assembler, so nothing to guard against
    }
    const UnrolledSource text = unrollSource(numeric, program, {{onlyLoop(program), 2}});
    EXPECT_FALSE(text.ok);
    EXPECT_NE(text.reason.find("numeric offset"), std::string::npos) << text.reason;
}

// ---------------------------------------------------------------------------
// Trip counts, and a loop entered several times
// ---------------------------------------------------------------------------

std::string sumLoop(int trips)
{
    return R"(.data
arr: .word 3, 1, 4, 1, 5, 9, 2, 6, 5, 3
out: .word 0
.text
    lui x10, 65536
    li x6, )" + std::to_string(trips) +
           R"(
    li x7, 0
loop:
    lw x5, 0(x10)
    add x7, x7, x5
    addi x10, x10, 4
    addi x6, x6, -1
    bnez x6, loop
    lui x11, 65536
    sw x7, 40(x11)
    li x17, 10
    ecall
)";
}

TEST(ReorderLoopUnroll, TripCounts)
{
    Kites::setupVmStateDirectory();
    for (int trips : {1, 2, 3, 4, 5, 8})
    {
        for (int k : {2, 4})
        {
            const std::string what = std::to_string(trips) + " trips, x" + std::to_string(k);
            expectUnrolledMatches<Kites::RV5StageProcessorHF>(sumLoop(trips), k, PipelineKind::HF, what);
            expectUnrolledMatches<Kites::RV5StageProcessorHNF>(sumLoop(trips), k, PipelineKind::HNF, what);
        }
    }
}

TEST(ReorderLoopUnroll, LoopEnteredSeveralTimes)
{
    Kites::setupVmStateDirectory();
    // The inner loop is entered 4 times, with 4, 3, 2 and 1 trips: every copy
    // gets to be the one the loop leaves from.
    const std::string source = R"(.data
arr: .word 3, 1, 4, 1, 5, 9, 2, 6, 5, 3
.text
    li x9, 4
    li x7, 0
outer:
    lui x10, 65536
    mv x6, x9
inner:
    lw x5, 0(x10)
    add x7, x7, x5
    addi x10, x10, 4
    addi x6, x6, -1
    bnez x6, inner
    addi x9, x9, -1
    bnez x9, outer
    li x17, 10
    ecall
)";
    for (int k : {2, 4})
    {
        expectUnrolledMatches<Kites::RV5StageProcessorHF>(source, k, PipelineKind::HF, "nested, rv5-h-f");
        expectUnrolledMatches<Kites::RV5StageProcessorHNF>(source, k, PipelineKind::HNF, "nested, rv5-h-nf");
    }
}

// ---------------------------------------------------------------------------
// Predict -> transform -> verify
// ---------------------------------------------------------------------------

TEST(ReorderLoopUnroll, KeepsTheCombinationOrTheBestSingleLoop)
{
    std::vector<LoopUnrollRecord> records(3);
    records[0].measured = 5;
    records[1].measured = 9;
    records[2].measured = 9;
    EXPECT_EQ(pickUnrolledLoops(records, {0, 1, 2}, true), (std::vector<size_t>{0, 1, 2}));
    EXPECT_EQ(pickUnrolledLoops(records, {0, 1, 2}, false), std::vector<size_t>{1});   // earlier of the two 9s
    EXPECT_EQ(pickUnrolledLoops(records, {0, 2}, false), std::vector<size_t>{2});
    EXPECT_EQ(pickUnrolledLoops(records, {2}, false), std::vector<size_t>{2});
    EXPECT_TRUE(pickUnrolledLoops(records, {}, false).empty());
}

// Two loops that each pass on their own are combined, and the combination is
// checked and kept.
TEST(ReorderLoopUnroll, TwoLoopsAreUnrolledTogether)
{
    Kites::setupVmStateDirectory();
    const std::string source = R"(
    li x5, 5
first:
    addi x7, x7, 2
    addi x5, x5, -1
    bgtz x5, first
    li x6, 3
second:
    addi x8, x8, 5
    addi x6, x6, -1
    bnez x6, second
    li x17, 10
    ecall
)";
    const Kites::AssembledProgram program = assembleText(source);
    const UnrollOutcome f = unrollAndReorder<Kites::RV5StageProcessorHF>(source, program, {}, PipelineKind::HF);
    const UnrollOutcome n = unrollAndReorder<Kites::RV5StageProcessorHNF>(source, program, {}, PipelineKind::HNF);
    for (const UnrollOutcome *o : {&f, &n})
    {
        ASSERT_EQ(o->loops.size(), 2u);
        EXPECT_TRUE(o->loops[0].kept && o->loops[1].kept) << unrollSuggestions(*o);
        EXPECT_EQ(o->note, "unrolled 2 loops together");
        EXPECT_TRUE(Kites::sameState(o->original, o->final_));
        EXPECT_EQ(o->final_.retired, o->original.retired);
        EXPECT_LT(o->final_.cycles, o->reorderOnly.cycles);
    }
}

// The whole pipeline on one program: the suggestion line, and the written
// assembly re-assembles to a program with the original's final state.
TEST(ReorderLoopUnroll, OutputReassemblesAndSuggestionsNameTheLoop)
{
    Kites::setupVmStateDirectory();
    const std::string source = sumLoop(10);
    const Kites::AssembledProgram program = assembleText(source);
    const UnrollOutcome o = unrollAndReorder<Kites::RV5StageProcessorHNF>(source, program, {}, PipelineKind::HNF);
    ASSERT_TRUE(o.unrolled) << o.note << " / " << (o.loops.empty() ? "" : o.loops.front().reason);
    ASSERT_EQ(o.loops.size(), 1u);
    const LoopUnrollRecord &r = o.loops.front();
    EXPECT_TRUE(r.kept);
    EXPECT_GT(r.measured, 0);
    EXPECT_LT(o.final_.cycles, o.reorderOnly.cycles);
    const std::string expected = "loop at lines " + std::to_string(r.firstLine) + "–" + std::to_string(r.lastLine) +
                                 " unrolled ×" + std::to_string(r.k) + ": predicted " + std::to_string(r.predicted) +
                                 " cycles saved, measured " + std::to_string(r.measured);
    EXPECT_NE(unrollSuggestions(o).find(expected), std::string::npos) << unrollSuggestions(o);

    const Kites::AssembledProgram written = assembleText(writeUnrolledAssembly(o));
    const Kites::RunResult again = runOn<Kites::RV5StageProcessorHNF>(written);
    EXPECT_TRUE(Kites::sameState(o.original, again));
    EXPECT_EQ(again.cycles, o.final_.cycles);
}

// ---------------------------------------------------------------------------
// Every benchmark, both pipelines: own seed plus 3 random seeds
// ---------------------------------------------------------------------------

namespace
{

struct BenchTotals
{
    long long original = 0, reorderOnly = 0, final_ = 0;
    int unrolled = 0, inputs = 0, notJudged = 0;
};

template <class VM>
void checkBenchmark(const std::string &name, const std::string &source, const Kites::AssembledProgram &program,
                    const Kites::Seed &seed, size_t programIndex, PipelineKind kind, BenchTotals &totals)
{
    const std::string where = name + " on " + (kind == PipelineKind::HF ? kName[0] : kName[1]);
    const UnrollOutcome o = unrollAndReorder<VM>(source, program, seed, kind);
    totals.original += o.original.cycles;
    totals.reorderOnly += o.reorderOnly.cycles;
    totals.final_ += o.final_.cycles;
    totals.unrolled += o.unrolled ? 1 : 0;
    EXPECT_TRUE(Kites::sameState(o.original, o.final_)) << where;
    EXPECT_EQ(o.final_.retired, o.original.retired) << where;
    EXPECT_LE(o.final_.cycles, o.reorderOnly.cycles) << where;
    const Kites::AssembledProgram final_ = Kites::withText(o.finalProgram, o.finalReport.words);
    if (o.unrolled)
    {
        // The replay's prediction equals the VM's cycles on the unrolled program.
        const ReplayResult replay = replayPath(o.finalProgram, o.finalReport.words, tracePath(o.finalProgram, seed), kind);
        EXPECT_EQ(replay.predictedCycles, o.final_.cycles) << where;
        EXPECT_EQ(replay.countedStalls(), o.final_.stalls) << where;
    }

    // Random starting states (fuzz.h), chosen for the original program. A
    // random input can turn a data-driven loop count into billions; those hit
    // a 20000-cycle cap (the benchmarks themselves need under 6000) and are
    // not judged, as in reorder-fuzz.
    const Kites::fuzz::FuzzProgram fp{name, program, seed};
    const Kites::fuzz::InputPlan plan = Kites::fuzz::analyzeInputs(program, seed);
    const unsigned randomCap = 20000;
    const unsigned window = Kites::reorder_report::kDataWindowWords;
    for (size_t input = 0; input < 3; ++input)
    {
        const Kites::Seed random = Kites::fuzz::randomSeed(
            fp, plan, Kites::fuzz::inputSeedFor(Kites::fuzz::kDefaultRngSeed, programIndex, input));
        const Kites::RunResult before = Kites::runProgram<VM>(program, random, randomCap, window);
        if (before.hitCap || !before.textIntact)
        {
            ++totals.notJudged;
            continue;
        }
        const Kites::RunResult after = Kites::runProgram<VM>(final_, random, randomCap, window);
        ++totals.inputs;
        EXPECT_TRUE(Kites::sameState(before, after) && after.textIntact) << where << ", random input " << input;
        EXPECT_EQ(after.retired, before.retired) << where << ", random input " << input;
    }
}

} // namespace

TEST(ReorderLoopUnroll, EveryBenchmarkKeepsItsFinalState)
{
    namespace be = Kites::bench_eval;
    Kites::setupVmStateDirectory();
    std::vector<be::FilterRow> filter;
    const std::vector<be::UsableProgram> usable = be::filterPrograms(be::findPrograms("../bench/programs"), filter);
    ASSERT_FALSE(usable.empty());
    BenchTotals totals[2];
    for (size_t i = 0; i < usable.size(); ++i)
    {
        const be::FilterRow &row = filter[usable[i].filterIndex];
        const std::string source = be::readFile(row.path);
        checkBenchmark<Kites::RV5StageProcessorHF>(row.name, source, usable[i].program, usable[i].seed, i,
                                                   PipelineKind::HF, totals[0]);
        checkBenchmark<Kites::RV5StageProcessorHNF>(row.name, source, usable[i].program, usable[i].seed, i,
                                                    PipelineKind::HNF, totals[1]);
    }
    for (int p = 0; p < 2; ++p)
    {
        std::cout << "[ UNROLL ] " << kName[p] << ": " << usable.size() << " benchmarks, " << totals[p].unrolled
                  << " unrolled; cycles original " << totals[p].original << ", reorder-only "
                  << totals[p].reorderOnly << ", unroll + reorder " << totals[p].final_ << "; random inputs checked "
                  << totals[p].inputs << ", not judged " << totals[p].notJudged << std::endl;
    }
}
