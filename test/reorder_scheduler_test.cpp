#include <gtest/gtest.h>

#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/scheduler.h"
#include "utils/utils.h"

using namespace Kites::reorder;

namespace
{

// ---------------------------------------------------------------------
// Hand encoders, same idea as in reorder_dep_graph_test.cpp.
// ---------------------------------------------------------------------
uint32_t addi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t add(int rd, int rs1, int rs2)  { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33; }
uint32_t lw(int rd, int rs1, int imm)   { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03; }

bool respectsDepGraph(const std::vector<size_t> &order, const DepGraph &g)
{
    std::vector<size_t> position(order.size());
    for (size_t pos = 0; pos < order.size(); ++pos)
    {
        position[order[pos]] = pos;
    }
    for (size_t i = 0; i < g.size; ++i)
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

// ---------------------------------------------------------------------
// Randomized straight-line program generator, same safe-base-address
// pattern as reorder_cost_model_test.cpp (bases well clear of the text
// region, so a generated store can never clobber a later instruction word).
// ---------------------------------------------------------------------
std::string makeRandomProgram(std::mt19937 &rng, int bodyLen)
{
    static const int basesReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const int offsets[] = {0, 4, 8, 12, 16};
    static const char *rtypeOps[] = {"add", "sub", "and", "or", "xor", "sll",
                                     "srl", "sra", "mul", "div", "rem"};

    std::uniform_int_distribution<int> pickPool(0, static_cast<int>(std::size(pool)) - 1);
    std::uniform_int_distribution<int> pickBase(0, static_cast<int>(std::size(basesReg)) - 1);
    std::uniform_int_distribution<int> pickOffset(0, static_cast<int>(std::size(offsets)) - 1);
    std::uniform_int_distribution<int> pickOp(0, 13);
    std::uniform_int_distribution<int> coinFlip(0, 1);
    std::uniform_int_distribution<int> randImm(0, 2047);

    std::ostringstream out;
    for (int i : {0, 1, 2})
    {
        out << "addi x" << basesReg[i] << ", x0, " << baseAddr[i] << "\n";
    }

    for (int i = 0; i < bodyLen; ++i)
    {
        int op = pickOp(rng);
        if (op == 0)
        {
            int rd = pool[pickPool(rng)];
            int rs1 = pool[pickPool(rng)];
            int imm = randImm(rng);
            if (coinFlip(rng))
            {
                int collideWith = pool[pickPool(rng)];
                imm = (imm & ~0x1F) | collideWith;
            }
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else if (op == 12)
        {
            int rd = pool[pickPool(rng)];
            int base = basesReg[pickBase(rng)];
            int off = offsets[pickOffset(rng)];
            out << "lw x" << rd << ", " << off << "(x" << base << ")\n";
        }
        else if (op == 13)
        {
            int rs2 = pool[pickPool(rng)];
            int base = basesReg[pickBase(rng)];
            int off = offsets[pickOffset(rng)];
            out << "sw x" << rs2 << ", " << off << "(x" << base << ")\n";
        }
        else
        {
            int rd = pool[pickPool(rng)];
            int rs1 = pool[pickPool(rng)];
            int rs2 = pool[pickPool(rng)];
            out << rtypeOps[op - 1] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

// All 15 addresses the generator above can ever touch.
std::vector<uint64_t> randomProgramMemAddrs()
{
    std::vector<uint64_t> addrs;
    for (int base : {1000, 1200, 1400})
    {
        for (int off : {0, 4, 8, 12, 16})
        {
            addrs.push_back(static_cast<uint64_t>(base + off));
        }
    }
    return addrs;
}

// ---------------------------------------------------------------------
// VM run + equivalence-check helpers, reused by the randomized and the
// branchy / loop equivalence tests.
// ---------------------------------------------------------------------
struct RunResult
{
    unsigned totalStalls = 0;
    unsigned instructionsRetired = 0;
    std::vector<uint64_t> gprs;       // x1..x31
    std::vector<uint32_t> memWords;   // at the caller's chosen addresses
};

template <typename VmType>
RunResult runProgram(const Kites::AssembledProgram &program, const std::vector<uint64_t> &memAddrs)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<VmType>();
    vm->LoadProgram(program);
    vm->DebugRun();

    RunResult result;
    result.totalStalls = vm->total_stalls_;
    result.instructionsRetired = vm->instructions_retired_;
    result.gprs.reserve(31);
    for (int r = 1; r <= 31; ++r)
    {
        result.gprs.push_back(vm->registers_.ReadGpr(r));
    }
    result.memWords.reserve(memAddrs.size());
    for (uint64_t addr : memAddrs)
    {
        result.memWords.push_back(vm->memory_controller_.readWord(addr));
    }
    return result;
}

const char *pipelineName(PipelineKind kind)
{
    return kind == PipelineKind::HF ? "h_f" : "h_nf";
}

// Runs `originalProgram` as-is and after scheduleProgram(), on VmType, and
// checks they agree on every architecturally-visible outcome. Returns the
// before/after stall numbers (predicted and real) for reporting.
template <typename VmType>
void checkEquivalence(const std::string &name, PipelineKind kind,
                      const Kites::AssembledProgram &originalProgram,
                      const std::vector<uint64_t> &memAddrs,
                      long long &reportedPredictedBefore, long long &reportedPredictedAfter,
                      long long &reportedVmBefore, long long &reportedVmAfter)
{
    RunResult before = runProgram<VmType>(originalProgram, memAddrs);

    ScheduleReport report = scheduleProgram(originalProgram.text_buffer, kind,
                                           labelLeaders(originalProgram));
    Kites::AssembledProgram scheduledProgram = originalProgram;
    scheduledProgram.text_buffer = report.words;
    RunResult after = runProgram<VmType>(scheduledProgram, memAddrs);

    EXPECT_EQ(before.gprs, after.gprs) << name << " (" << pipelineName(kind) << "): GPRs differ";
    EXPECT_EQ(before.memWords, after.memWords)
        << name << " (" << pipelineName(kind) << "): data memory differs";
    EXPECT_EQ(before.instructionsRetired, after.instructionsRetired)
        << name << " (" << pipelineName(kind) << "): instructions_retired_ differs";
    EXPECT_LE(after.totalStalls, before.totalStalls)
        << name << " (" << pipelineName(kind) << "): scheduled VM is worse than original";

    std::cout << "[SCHED-REPORT] " << name << " (" << pipelineName(kind) << ")"
              << " predicted stalls: " << report.stallsBefore << " -> " << report.stallsAfter
              << " | VM total_stalls_: " << before.totalStalls << " -> " << after.totalStalls
              << " | moves=" << report.moves.size() << std::endl;

    reportedPredictedBefore += report.stallsBefore;
    reportedPredictedAfter += report.stallsAfter;
    reportedVmBefore += before.totalStalls;
    reportedVmAfter += after.totalStalls;
}

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

} // namespace

// =======================================================================
// Hand-built: every scheduleBlock() output respects every dep-graph edge.
// =======================================================================

TEST(ReorderScheduler, RespectsDepGraphEdges_HandBuilt)
{
    // addi x1,x0,7 / add x2,x1,x1 / addi x3,x0,9 / add x4,x2,x3 / sub x5,x4,x1
    // Chain of RAW edges plus one independent instruction (addi x3).
    std::vector<uint32_t> words = {
        addi(1, 0, 7), add(2, 1, 1), addi(3, 0, 9), add(4, 2, 3), add(5, 4, 1)};
    Block block{0, words.size(), false};
    DepGraph g = buildDepGraph(words, block);

    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        std::vector<size_t> order = scheduleBlock(words, block, kind);
        EXPECT_TRUE(respectsDepGraph(order, g)) << "kind=" << pipelineName(kind);
        ASSERT_EQ(order.size(), words.size());
        std::vector<bool> seen(order.size(), false);
        for (size_t idx : order)
        {
            ASSERT_FALSE(seen[idx]);
            seen[idx] = true;
        }
    }
}

// =======================================================================
// Hand-built: load-use example where an independent instruction fills the
// slot, on both pipelines.
// =======================================================================

TEST(ReorderScheduler, LoadUse_HF_FillsSlot)
{
    // lw x5,0(x10) / add x6,x5,x5 / addi x7,x0,1 (independent of x5)
    std::vector<uint32_t> words = {lw(5, 10, 0), add(6, 5, 5), addi(7, 0, 1)};
    Block block{0, words.size(), false};

    std::vector<DecodedInst> originalInsts = {decode(words[0]), decode(words[1]), decode(words[2])};
    int before = predictStalls(originalInsts, PipelineKind::HF);

    std::vector<size_t> order = scheduleBlock(words, block, PipelineKind::HF);
    std::vector<DecodedInst> scheduledInsts;
    for (size_t idx : order)
    {
        scheduledInsts.push_back(decode(words[idx]));
    }
    int after = predictStalls(scheduledInsts, PipelineKind::HF);

    EXPECT_EQ(before, 1);
    EXPECT_EQ(after, 0);
    // The independent addi must land between the load and its use.
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0u);   // lw first (only legal choice)
    EXPECT_EQ(order[1], 2u);   // addi x7 fills the load-use slot
    EXPECT_EQ(order[2], 1u);   // add last
}

TEST(ReorderScheduler, LoadUse_HNF_FillsSlot)
{
    std::vector<uint32_t> words = {lw(5, 10, 0), add(6, 5, 5), addi(7, 0, 1)};
    Block block{0, words.size(), false};

    std::vector<DecodedInst> originalInsts = {decode(words[0]), decode(words[1]), decode(words[2])};
    int before = predictStalls(originalInsts, PipelineKind::HNF);

    std::vector<size_t> order = scheduleBlock(words, block, PipelineKind::HNF);
    std::vector<DecodedInst> scheduledInsts;
    for (size_t idx : order)
    {
        scheduledInsts.push_back(decode(words[idx]));
    }
    int after = predictStalls(scheduledInsts, PipelineKind::HNF);

    EXPECT_EQ(before, 2);
    EXPECT_EQ(after, 1);   // HNF still charges 1 cycle for a distance-2 hazard
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0u);
    EXPECT_EQ(order[1], 2u);
    EXPECT_EQ(order[2], 1u);
}

// =======================================================================
// Randomized, VM-free: scheduleBlock() is never worse, and always respects
// the dependency graph, over many random straight-line blocks.
// =======================================================================

namespace
{

void randomizedStructuralCheck(PipelineKind kind, unsigned seed, int trials)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> lenDist(5, 20);
    long long totalBefore = 0;
    long long totalAfter = 0;

    for (int t = 0; t < trials; ++t)
    {
        std::string source = makeRandomProgram(rng, lenDist(rng));
        Kites::AssembledProgram program = assembleSource(source);
        std::vector<Block> blocks = findBlocks(program.text_buffer);

        for (const Block &block : blocks)
        {
            DepGraph g = buildDepGraph(program.text_buffer, block);
            std::vector<size_t> order = scheduleBlock(program.text_buffer, block, kind);
            ASSERT_TRUE(respectsDepGraph(order, g)) << "trial " << t << "\n" << source;

            std::vector<DecodedInst> orig, sched;
            for (size_t k = block.begin; k < block.end; ++k)
            {
                orig.push_back(decode(program.text_buffer[k]));
            }
            for (size_t localIdx : order)
            {
                sched.push_back(decode(program.text_buffer[block.begin + localIdx]));
            }
            int before = predictStalls(orig, kind);
            int after = predictStalls(sched, kind);
            EXPECT_LE(after, before) << "trial " << t << "\n" << source;
            totalBefore += before;
            totalAfter += after;
        }
    }

    double reduction = totalBefore > 0 ? 100.0 * static_cast<double>(totalBefore - totalAfter) /
                                             static_cast<double>(totalBefore)
                                       : 0.0;
    std::cout << "[SCHED-REPORT] randomized structural (" << pipelineName(kind)
              << ") trials=" << trials << " predicted stalls: " << totalBefore << " -> " << totalAfter
              << " (" << reduction << "% reduction)" << std::endl;
}

} // namespace

TEST(ReorderScheduler, RandomizedStructural_HF) { randomizedStructuralCheck(PipelineKind::HF, 2024u, 300); }
TEST(ReorderScheduler, RandomizedStructural_HNF) { randomizedStructuralCheck(PipelineKind::HNF, 4048u, 300); }

// =======================================================================
// Randomized VM equivalence: 100 straight-line programs per mode. Original
// and scheduled must retire the same instructions, end with identical GPRs
// and data memory, and the scheduled VM must never need more stalls.
// =======================================================================

namespace
{

template <typename VmType>
void randomizedVmEquivalence(PipelineKind kind, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> lenDist(10, 20);
    std::vector<uint64_t> memAddrs = randomProgramMemAddrs();

    long long predictedBefore = 0, predictedAfter = 0;
    long long vmBefore = 0, vmAfter = 0;

    for (int trial = 0; trial < 100; ++trial)
    {
        std::string source = makeRandomProgram(rng, lenDist(rng));
        Kites::AssembledProgram program = assembleSource(source);
        checkEquivalence<VmType>("random#" + std::to_string(trial), kind, program, memAddrs,
                                predictedBefore, predictedAfter, vmBefore, vmAfter);
    }

    double reduction =
        vmBefore > 0 ? 100.0 * static_cast<double>(vmBefore - vmAfter) / static_cast<double>(vmBefore) : 0.0;
    std::cout << "[SCHED-REPORT] randomized VM equivalence (" << pipelineName(kind) << ") n=100"
              << " predicted: " << predictedBefore << " -> " << predictedAfter
              << " | VM total_stalls_: " << vmBefore << " -> " << vmAfter << " (" << reduction
              << "% reduction)" << std::endl;
}

} // namespace

TEST(ReorderScheduler, RandomizedVmEquivalence_HF)
{
    randomizedVmEquivalence<Kites::RV5StageProcessorHF>(PipelineKind::HF, 13579u);
}

TEST(ReorderScheduler, RandomizedVmEquivalence_HNF)
{
    randomizedVmEquivalence<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, 97531u);
}

// =======================================================================
// Branchy and loop equivalence, using labelLeaders() for extraLeaders.
// =======================================================================

namespace
{

struct ExampleProgram
{
    std::string name;
    std::string source;
    std::vector<uint64_t> memAddrs;
};

std::vector<ExampleProgram> branchyExamples()
{
    return {
        {"R_B_hazard",
         "addi x1, x0, 3\n"
         "add  x2, x1, x1\n"
         "beq  x2, x1, L1\n"
         "addi x10, x0, 0\n"
         "jal  x0, END\n"
         "L1:\n"
         "addi x10, x0, 1\n"
         "END:\n",
         {}},
        {"L_B_hazard",
         "addi x1, x0, 100\n"
         "addi x2, x0, 3\n"
         "sw  x2, 0(x1)\n"
         "lw   x3, 0(x1)\n"
         "beq  x2, x3, L1\n"
         "addi x7, x0, 0\n"
         "jal  x0, END\n"
         "L1:\n"
         "addi x7, x0, 1\n"
         "END:\n",
         {100}},
        // Array sum: pre-store 4 values, then loop-sum them.
        {"loop_array_sum",
         "addi x20, x0, 1000\n"
         "addi x21, x0, 4\n"
         "addi x22, x0, 0\n"
         "addi x23, x0, 0\n"
         "sw x21, 0(x20)\n"
         "sw x21, 4(x20)\n"
         "sw x21, 8(x20)\n"
         "sw x21, 12(x20)\n"
         "LOOP:\n"
         "beq x23, x21, END\n"
         "lw x24, 0(x20)\n"
         "add x22, x22, x24\n"
         "addi x20, x20, 4\n"
         "addi x23, x23, 1\n"
         "jal x0, LOOP\n"
         "END:\n",
         {1000, 1004, 1008, 1012}},
        // Load-use inside the loop body: lw immediately consumed, with two
        // independent bump instructions available to fill the slot.
        {"loop_load_use_body",
         "addi x20, x0, 1100\n"
         "addi x21, x0, 3\n"
         "addi x22, x0, 0\n"
         "sw x21, 0(x20)\n"
         "sw x21, 4(x20)\n"
         "sw x21, 8(x20)\n"
         "LOOP2:\n"
         "beq x22, x21, END2\n"
         "lw x24, 0(x20)\n"
         "add x25, x24, x24\n"
         "addi x20, x20, 4\n"
         "addi x22, x22, 1\n"
         "jal x0, LOOP2\n"
         "END2:\n",
         {1100, 1104, 1108}},
        // Store then load at the SAME address each iteration: the dep graph
        // must keep them ordered even after scheduling.
        {"loop_store_then_load",
         "addi x20, x0, 1200\n"
         "addi x21, x0, 3\n"
         "addi x22, x0, 0\n"
         "addi x23, x0, 0\n"
         "LOOP3:\n"
         "beq x22, x21, END3\n"
         "sw x22, 0(x20)\n"
         "lw x24, 0(x20)\n"
         "add x23, x23, x24\n"
         "addi x20, x20, 4\n"
         "addi x22, x22, 1\n"
         "jal x0, LOOP3\n"
         "END3:\n",
         {1200, 1204, 1208}},
    };
}

// The five original straight-line baseline hazard examples (see
// reorder_cost_model_test.cpp), reused here purely for the stall-reduction
// report asked for on examples/*_hazard.s.
std::vector<ExampleProgram> baselineExamples()
{
    return {
        {"L_U_hazard",
         "addi x1, x0, 100\naddi x2, x0, 20\nsw   x2, 0(x1)\nlw   x3, 0(x1)\nadd  x6, x3, x1\n",
         {100}},
        {"R_M_hazard",
         "addi x3, x0, 55\naddi x4, x0, 400\nsw   x3, 0(x4)\naddi x1, x0, 200\nadd  x2, x1, x1\n"
         "lw   x9, 0(x2)\n",
         {400}},
        {"L_S_hazard",
         "addi x1, x0, 100\naddi x2, x0, 7\nsw   x2, 0(x1)\nlw   x3, 0(x1)\naddi x3, x0, 300\n"
         "sw   x2, 0(x3)\nlw   x7, 0(x3)\n",
         {100, 300}},
        {"R_S_hazard",
         "addi x1, x0, 10\nadd  x2, x1, x1\naddi x3, x0, 200\nsw   x2, 0(x3)\nlw   x8, 0(x3)\n",
         {200}},
        {"R_R_hazard", "addi x1, x0, 5\nadd  x2, x1, x1\nsub  x6, x2, x1\n", {}},
    };
}

template <typename VmType>
void runExampleSet(const std::vector<ExampleProgram> &examples, PipelineKind kind,
                   long long &predictedBefore, long long &predictedAfter, long long &vmBefore,
                   long long &vmAfter)
{
    for (const ExampleProgram &ex : examples)
    {
        Kites::AssembledProgram program = assembleSource(ex.source);
        checkEquivalence<VmType>(ex.name, kind, program, ex.memAddrs, predictedBefore,
                                predictedAfter, vmBefore, vmAfter);
    }
}

} // namespace

TEST(ReorderScheduler, BranchyAndLoopEquivalence_HF)
{
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    runExampleSet<Kites::RV5StageProcessorHF>(branchyExamples(), PipelineKind::HF, predictedBefore,
                                              predictedAfter, vmBefore, vmAfter);
    std::cout << "[SCHED-REPORT] branchy+loop total (h_f) predicted: " << predictedBefore << " -> "
              << predictedAfter << " | VM: " << vmBefore << " -> " << vmAfter << std::endl;
}

TEST(ReorderScheduler, BranchyAndLoopEquivalence_HNF)
{
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    runExampleSet<Kites::RV5StageProcessorHNF>(branchyExamples(), PipelineKind::HNF, predictedBefore,
                                               predictedAfter, vmBefore, vmAfter);
    std::cout << "[SCHED-REPORT] branchy+loop total (h_nf) predicted: " << predictedBefore << " -> "
              << predictedAfter << " | VM: " << vmBefore << " -> " << vmAfter << std::endl;
}

TEST(ReorderScheduler, BaselineExamplesStallReport_HF)
{
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    runExampleSet<Kites::RV5StageProcessorHF>(baselineExamples(), PipelineKind::HF, predictedBefore,
                                              predictedAfter, vmBefore, vmAfter);
    std::cout << "[SCHED-REPORT] baseline examples total (h_f) predicted: " << predictedBefore
              << " -> " << predictedAfter << " | VM: " << vmBefore << " -> " << vmAfter << std::endl;
}

TEST(ReorderScheduler, BaselineExamplesStallReport_HNF)
{
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    runExampleSet<Kites::RV5StageProcessorHNF>(baselineExamples(), PipelineKind::HNF, predictedBefore,
                                               predictedAfter, vmBefore, vmAfter);
    std::cout << "[SCHED-REPORT] baseline examples total (h_nf) predicted: " << predictedBefore
              << " -> " << predictedAfter << " | VM: " << vmBefore << " -> " << vmAfter << std::endl;
}
