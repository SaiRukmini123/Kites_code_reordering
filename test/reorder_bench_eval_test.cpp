// Tests for reorder/bench_eval.h: the benchmark header, the validity filter, near-duplicate
// removal, the classification of blocks whose stalls remain, and the committed benchmark set.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "reorder/bench_eval.h"
#include "reorder_hazard_cases.h"

using namespace Kites;
using namespace Kites::bench_eval;
using Kites::reorder::PipelineKind;

namespace
{
const std::string kHeader = "# name: t\n# category: hazard\n# source: llm\n# inputs: none\n";
const std::string kExit = "li a7, 10\necall\n";

// Writes `programs` (name -> text) into a fresh directory and runs the batch over it.
BenchRun runOn(const std::vector<std::pair<std::string, std::string>> &programs)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "kites_bench_eval_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::vector<std::filesystem::path> paths;
    for (const auto &[name, text] : programs)
    {
        paths.push_back(dir / (name + ".s"));
        std::ofstream(paths.back()) << text;
    }
    hazard_bench::assembleHazardText(""); // sets up the VM state directory
    return runBench(paths);
}

std::string problemsOf(const FilterRow &row)
{
    std::string all;
    for (const std::string &p : row.problems)
    {
        all += p + "\n";
    }
    return all;
}

// Classifies the blocks of `source` (already in benchmark form) for `kind`.
std::vector<BlockOutcome> classify(const std::string &source, PipelineKind kind, const Seed &seed)
{
    AssembledProgram program = hazard_bench::assembleHazardText(source);
    auto outcome = reorder_report::runReorder(program, kind, seed, kBenchCycleCap);
    EXPECT_TRUE(outcome.equivalent);
    return classifyBlocks(program, outcome.report, kind);
}

const BlockOutcome *firstWithStallsLeft(const std::vector<BlockOutcome> &blocks)
{
    for (const BlockOutcome &b : blocks)
    {
        if (b.stallsAfter > 0)
        {
            return &b;
        }
    }
    return nullptr;
}
} // namespace

TEST(ReorderBenchEval, ParsesHeader)
{
    ProgramHeader h = parseProgramHeader("# name: sum\n#category:loop\n# A prose comment: ignored\n"
                                         "# inputs: x10=0x10\n.text\n# name: later\naddi x1,x0,1\n");
    EXPECT_EQ(h.get("name"), "sum");
    EXPECT_EQ(h.get("category"), "loop");
    EXPECT_EQ(h.get("inputs"), "x10=0x10");
    EXPECT_FALSE(h.has("a prose comment"));
}

TEST(ReorderBenchEval, ValidProgramPasses)
{
    BenchRun run = runOn({{"ok", kHeader + ".data\nv: .word 7\n.text\nla a0, v\nlw t0, 0(a0)\n"
                                           "add t1, t0, t0\nsw t1, 0(a0)\n" + kExit}});
    ASSERT_EQ(run.filter.size(), 1u);
    EXPECT_TRUE(run.filter[0].usable()) << problemsOf(run.filter[0]);
    EXPECT_EQ(run.results.size(), 2u); // both VMs
}

TEST(ReorderBenchEval, FilterRejectsEachKindOfProblem)
{
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"no_category", "# name: t\n# source: llm\n# inputs: none\naddi t0, x0, 1\n" + kExit},
        {"no_inputs", "# name: t\n# category: loop\n# source: llm\naddi t0, x0, 1\n" + kExit},
        {"no_exit", kHeader + "addi t0, x0, 1\n"},
        {"print_syscall", kHeader + "li a0, 5\nli a7, 1\necall\n" + kExit},
        {"exit_code_wrong", kHeader + "li a7, 1\necall\n"},
        {"uses_sp", kHeader + "addi sp, sp, -16\n" + kExit},
        {"infinite_loop", kHeader + "spin:\nj spin\n" + kExit},
        {"overwrites_code", kHeader + "li t0, 0x13\nsw t0, 0(x0)\n" + kExit},
        {"does_not_assemble", kHeader + "add t0, t1\n" + kExit},
        {"hang_label", kHeader + ".data\ndata: .word 1\n.text\nla a0, data\n" + kExit},
    };
    const std::vector<std::string> expected = {
        "category", "inputs", "does not end with the exit ecall", "ecall used before the end",
        "a7 is not set to 10", "uses sp", "cycle cap", "overwrites its own instructions",
        "does not assemble", "hangs on it"};
    BenchRun run = runOn(bad);
    ASSERT_EQ(run.filter.size(), bad.size());
    for (size_t i = 0; i < bad.size(); ++i)
    {
        // findPrograms is not used here, so the order is the input order
        SCOPED_TRACE(bad[i].first);
        EXPECT_FALSE(run.filter[i].valid);
        EXPECT_NE(problemsOf(run.filter[i]).find(expected[i]), std::string::npos)
            << problemsOf(run.filter[i]);
    }
    EXPECT_TRUE(run.results.empty());
}

TEST(ReorderBenchEval, NearDuplicatesAreRemoved)
{
    // Same instruction shape, different registers and constants.
    const std::string a = kHeader + "li t0, 1\nli t1, 2\nadd t2, t0, t1\nadd t3, t2, t2\n"
                                    "sub t4, t3, t0\nslli t5, t4, 1\nor t6, t5, t1\nxor s0, t6, t0\n" + kExit;
    const std::string b = kHeader + "li a1, 9\nli a2, 4\nadd a3, a1, a2\nadd a4, a3, a3\n"
                                    "sub a5, a4, a1\nslli a6, a5, 2\nor s1, a6, a2\nxor s2, s1, a1\n" + kExit;
    BenchRun run = runOn({{"a", a}, {"b", b}});
    ASSERT_EQ(run.filter.size(), 2u);
    EXPECT_TRUE(run.filter[0].usable());
    EXPECT_TRUE(run.filter[1].valid);
    EXPECT_EQ(run.filter[1].duplicateOf, "t");
    EXPECT_FALSE(run.filter[1].usable());
    EXPECT_DOUBLE_EQ(shapeSimilarity(mnemonicSequence({0x13, 0x33}), mnemonicSequence({0x33, 0x13})),
                     0.0);
}

TEST(ReorderBenchEval, ClassifiesBlockBoundary)
{
    // The branch ends the block right after the load: nothing can go between them.
    auto blocks = classify("lw x5, 0(x10)\nbeq x5, x0, done\naddi x6, x0, 32\ndone:\naddi x7, x0, 64\n",
                           PipelineKind::HF, hazard_bench::hazardSeed());
    const BlockOutcome *b = firstWithStallsLeft(blocks);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->status, "unimproved");
    EXPECT_EQ(b->reason, StallReason::BlockBoundary);
}

TEST(ReorderBenchEval, ClassifiesDependencyChain)
{
    auto blocks = classify("addi x5, x0, 64\nadd x6, x5, x5\nadd x7, x6, x6\nadd x8, x7, x7\n",
                           PipelineKind::HNF, {});
    const BlockOutcome *b = firstWithStallsLeft(blocks);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->stallsAfter, 6);
    EXPECT_EQ(b->reason, StallReason::DependencyChain);
}

TEST(ReorderBenchEval, ClassifiesMemoryOrdering)
{
    // The store through x11 could fill the load-use gap, but x11 and x10 may alias, so it must
    // stay before the load.
    Seed seed;
    seed.regs = {{10, 0x10000000}, {11, 0x10000010}};
    auto blocks = classify("sw x6, 0(x11)\nlw x5, 0(x10)\nadd x7, x5, x5\n", PipelineKind::HF, seed);
    const BlockOutcome *b = firstWithStallsLeft(blocks);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->reason, StallReason::MemoryOrdering);
}

// A free, independent filler is in the dependency graph, yet one stall is left. Without a search
// over other orders the classifier does not claim the schedule is or is not optimal.
TEST(ReorderBenchEval, ClassifiesNotProvenOptimal)
{
    auto blocks = classify("addi x5, x0, 64\nadd x6, x5, x5\nadd x7, x6, x6\n"
                           "addi x11, x0, 32\nadd x12, x11, x11\nadd x13, x12, x12\n",
                           PipelineKind::HNF, {});
    const BlockOutcome *b = firstWithStallsLeft(blocks);
    ASSERT_NE(b, nullptr);
    EXPECT_LT(b->stallsAfter, b->stallsBefore);
    EXPECT_EQ(b->reason, StallReason::OtherNotProvenOptimal);
}

TEST(ReorderBenchEval, ReasonNames)
{
    EXPECT_STREQ(stallReasonName(StallReason::BlockBoundary), "block_boundary");
    EXPECT_STREQ(stallReasonName(StallReason::DependencyChain), "dependency_chain");
    EXPECT_STREQ(stallReasonName(StallReason::NothingIndependent), "nothing_independent");
    EXPECT_STREQ(stallReasonName(StallReason::MemoryOrdering), "memory_ordering");
    EXPECT_STREQ(stallReasonName(StallReason::FalseStall), "false_stall_immediate");
    EXPECT_STREQ(stallReasonName(StallReason::OtherNotProvenOptimal), "other_not_proven_optimal");
}

TEST(ReorderBenchEval, CsvFieldsAreQuoted)
{
    EXPECT_EQ(csvField("plain"), "plain");
    EXPECT_EQ(csvField("a,b"), "\"a,b\"");
    EXPECT_EQ(csvField("say \"hi\""), "\"say \"\"hi\"\"\"");
}

// The committed benchmark set: everything the filter keeps is equivalent after reordering on
// both pipelines, and the set meets the size target.
TEST(ReorderBenchEval, CommittedBenchmarkSet)
{
    const std::filesystem::path root = "../bench/programs";
    ASSERT_TRUE(std::filesystem::is_directory(root)) << "run from the test/ directory";
    BenchRun run = runBench(findPrograms(root));

    size_t usable = 0;
    for (const FilterRow &f : run.filter)
    {
        usable += f.usable() ? 1 : 0;
        if (!f.usable())
        {
            // The one known reject: an LLM-written program whose `data:` label hangs the assembler.
            EXPECT_EQ(f.name, "mixed_checksum") << problemsOf(f);
        }
    }
    EXPECT_GE(usable, 50u);
    for (const ResultRow &r : run.results)
    {
        SCOPED_TRACE(r.name + " on " + reorder_report::vmName(r.kind));
        EXPECT_TRUE(r.outcome.equivalent);
        EXPECT_FALSE(r.outcome.before.hitCap);
        EXPECT_EQ(r.outcome.before.retired, r.outcome.after.retired);
        EXPECT_LE(keptCycles(r.outcome), r.outcome.before.cycles);
    }
}
