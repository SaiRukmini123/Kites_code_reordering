#include <gtest/gtest.h>

#include <iostream>
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
// Hand encoders, same idea as in reorder_dep_graph_test.cpp. load()/store()
// take an explicit funct3 so every access width (byte/half/word/double) is
// reachable from one encoder each.
// ---------------------------------------------------------------------
uint32_t addi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }

uint32_t load(int rd, int rs1, int imm, int funct3)
{
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (static_cast<uint32_t>(funct3) << 12) | (rd << 7) | 0x03;
}

uint32_t store(int rs2, int rs1, int imm, int funct3)
{
    return (((imm >> 5) & 0x7F) << 25) | (static_cast<uint32_t>(rs2) << 20) | (rs1 << 15) |
           (static_cast<uint32_t>(funct3) << 12) | ((imm & 0x1F) << 7) | 0x23;
}

constexpr int kLB = 0, kLH = 1, kLW = 2, kLD = 3, kLBU = 4, kLHU = 5, kLWU = 6;
constexpr int kSB = 0, kSH = 1, kSW = 2, kSD = 3;

DepGraph graphOf(const std::vector<uint32_t> &words)
{
    return buildDepGraph(words, Block{0, words.size(), false});
}

} // namespace

// =======================================================================
// Unit tests: same-base disjoint/overlap, different base, base rewritten,
// self-base-write, access sizes, x0 base.
// =======================================================================

TEST(ReorderMemDeps, DisjointSameBase_NoEdge)
{
    // sw x2,0(x10) [0,4) ; lb x3,8(x10) [8,9) -- disjoint, same base x10.
    auto g = graphOf({store(2, 10, 0, kSW), load(3, 10, 8, kLB)});
    EXPECT_FALSE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, OverlappingSameBase_Edge)
{
    // Exactly the user's example: sw 0 then lb 3 -- [0,4) and [3,4) overlap at byte 3.
    auto g = graphOf({store(2, 10, 0, kSW), load(3, 10, 3, kLB)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, DifferentBases_Edge)
{
    // Same numeric offsets that WOULD be disjoint, but different base
    // registers -- we can't prove the addresses differ, so keep the edge.
    auto g = graphOf({store(2, 10, 0, kSW), load(3, 11, 8, kLB)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, BaseRewrittenBetween_Edge)
{
    // sw x2,0(x10) ; addi x10,x0,999 (redefines the base!) ; lb x3,8(x10)
    // Offsets look disjoint, but the base's value differs between the two
    // memory ops, so the direct edge must still be added.
    std::vector<uint32_t> words = {store(2, 10, 0, kSW), addi(10, 0, 999), load(3, 10, 8, kLB)};
    auto g = graphOf(words);
    EXPECT_TRUE(g.hasEdge(0, 2));
}

TEST(ReorderMemDeps, LoadWritesOwnBase_Edge)
{
    // lw x5,0(x5) ; lb x6,8(x5) -- the second op's base is the FIRST op's
    // result, so this is actually an ordinary RAW on x5, not a memory-only
    // link; linkedOnlyByMemory() must say so and the edge must be kept.
    auto g = graphOf({load(5, 5, 0, kLW), load(6, 5, 8, kLB)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, StoreThenLoadIntoOwnBase_Edge)
{
    // sw x2,0(x10) ; lw x10,8(x10) -- later writes the very base earlier
    // read. Ordinary WAR on x10 must keep this ordered too (if the scheduler
    // ran the load first, the store would then use the load's result as its
    // base instead of the original address).
    auto g = graphOf({store(2, 10, 0, kSW), load(10, 10, 8, kLW)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, DisjointSizes_ByteHalfWordDouble_NoEdge)
{
    EXPECT_FALSE(graphOf({store(1, 10, 0, kSB), store(1, 10, 2, kSH)}).hasEdge(0, 1));   // [0,1) [2,4)
    EXPECT_FALSE(graphOf({store(1, 10, 0, kSH), store(1, 10, 4, kSW)}).hasEdge(0, 1));   // [0,2) [4,8)
    EXPECT_FALSE(graphOf({store(1, 10, 0, kSW), store(1, 10, 8, kSD)}).hasEdge(0, 1));   // [0,4) [8,16)
    EXPECT_FALSE(graphOf({load(2, 10, 0, kLWU), store(1, 10, 4, kSB)}).hasEdge(0, 1));   // [0,4) [4,5)
}

TEST(ReorderMemDeps, OverlappingSizes_Edge)
{
    // sd at [0,8) and sw at [4,8) overlap.
    EXPECT_TRUE(graphOf({store(1, 10, 0, kSD), store(1, 10, 4, kSW)}).hasEdge(0, 1));
    // lhu at [2,4) and sb at [3,4) overlap.
    EXPECT_TRUE(graphOf({load(2, 10, 2, kLHU), store(1, 10, 3, kSB)}).hasEdge(0, 1));
}

TEST(ReorderMemDeps, X0Base_DisjointNoEdge)
{
    // Base x0 can never be rewritten (writes to x0 are discarded), so the
    // drop only needs disjoint ranges.
    auto g = graphOf({store(2, 0, 0, kSW), load(3, 0, 8, kLB)});
    EXPECT_FALSE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, X0Base_OverlapKeepsEdge)
{
    auto g = graphOf({store(2, 0, 0, kSW), load(3, 0, 2, kLB)});
    EXPECT_TRUE(g.hasEdge(0, 1));
}

TEST(ReorderMemDeps, SwitchOff_RestoresConservativeEdge)
{
    std::vector<uint32_t> words = {store(2, 10, 0, kSW), load(3, 10, 8, kLB)};
    Block block{0, words.size(), false};
    EXPECT_FALSE(buildDepGraph(words, block).hasEdge(0, 1));          // default: on
    EXPECT_FALSE(buildDepGraph(words, block, true).hasEdge(0, 1));
    EXPECT_TRUE(buildDepGraph(words, block, false).hasEdge(0, 1));
}

// =======================================================================
// Edge-removal count + stall report: how many conservative memory edges
// does this change actually drop, and what does that do to predicted and
// real scheduler stalls, on the loop programs, the baseline hazard
// examples, and the randomized memory-heavy set below.
// =======================================================================

namespace
{

int countDroppableMemoryEdges(const Kites::AssembledProgram &program)
{
    int dropped = 0;
    for (const Block &block : findBlocks(program.text_buffer, labelLeaders(program)))
    {
        std::vector<DecodedInst> insts;
        for (size_t k = block.begin; k < block.end; ++k)
        {
            insts.push_back(decode(program.text_buffer[k]));
        }
        for (size_t i = 0; i < insts.size(); ++i)
        {
            for (size_t j = i + 1; j < insts.size(); ++j)
            {
                if (mustStayAfter(insts[i], insts[j]) && linkedOnlyByMemory(insts[i], insts[j]) &&
                    canDropMemoryEdge(insts, i, j))
                {
                    ++dropped;
                }
            }
        }
    }
    return dropped;
}

// ---------------------------------------------------------------------
// VM run + equivalence-check helpers (same shape as reorder_scheduler_test.cpp).
// ---------------------------------------------------------------------
struct RunResult
{
    unsigned totalStalls = 0;
    unsigned instructionsRetired = 0;
    std::vector<uint64_t> gprs;
    std::vector<uint32_t> memWords;
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

const char *pipelineName(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

template <typename VmType>
void checkEquivalence(const std::string &name, PipelineKind kind,
                      const Kites::AssembledProgram &originalProgram,
                      const std::vector<uint64_t> &memAddrs, int &droppedEdges,
                      long long &predictedBefore, long long &predictedAfter, long long &vmBefore,
                      long long &vmAfter)
{
    droppedEdges += countDroppableMemoryEdges(originalProgram);

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

    predictedBefore += report.stallsBefore;
    predictedAfter += report.stallsAfter;
    vmBefore += before.totalStalls;
    vmAfter += after.totalStalls;
}

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

// ---------------------------------------------------------------------
// Memory-heavy randomized generator: loads/stores of mixed sizes off one or
// two bases in the data region, small offsets aligned to their own size,
// occasionally redefining a base midway. Safe base addresses (3000/3100/
// 3200/3300), well clear of the text region, each with its own 16-byte
// window so a redefinition never bleeds into another base's data.
// ---------------------------------------------------------------------
struct LoadOp { const char *mnemonic; int size; };
struct StoreOp { const char *mnemonic; int size; };

const LoadOp kLoadOps[] = {{"lb", 1}, {"lh", 2}, {"lw", 4}, {"ld", 8}, {"lbu", 1}, {"lhu", 2}, {"lwu", 4}};
const StoreOp kStoreOps[] = {{"sb", 1}, {"sh", 2}, {"sw", 4}, {"sd", 8}};
const int kAddrPool[] = {1000, 1100, 1200, 1300};
const int kDataPool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19};

std::vector<int> alignedOffsets(int size)
{
    std::vector<int> offs;
    for (int off = 0; off + size <= 16; off += size)
    {
        offs.push_back(off);
    }
    return offs;
}

std::string makeMemHeavyProgram(std::mt19937 &rng, int bodyLen)
{
    std::uniform_int_distribution<int> pickBool(0, 1);
    std::uniform_int_distribution<int> pickAddr(0, static_cast<int>(std::size(kAddrPool)) - 1);
    std::uniform_int_distribution<int> pickData(0, static_cast<int>(std::size(kDataPool)) - 1);
    std::uniform_int_distribution<int> pickLoadOp(0, static_cast<int>(std::size(kLoadOps)) - 1);
    std::uniform_int_distribution<int> pickStoreOp(0, static_cast<int>(std::size(kStoreOps)) - 1);
    std::uniform_int_distribution<int> pickStep(0, 99);
    static const char *aluOps[] = {"add", "sub", "and", "or", "xor"};
    std::uniform_int_distribution<int> pickAlu(0, static_cast<int>(std::size(aluOps)) - 1);

    const bool twoBases = pickBool(rng) == 1;
    std::vector<int> baseRegs = twoBases ? std::vector<int>{10, 11} : std::vector<int>{10};
    std::uniform_int_distribution<int> pickBase(0, static_cast<int>(baseRegs.size()) - 1);

    std::ostringstream out;
    for (int baseReg : baseRegs)
    {
        out << "addi x" << baseReg << ", x0, " << kAddrPool[pickAddr(rng)] << "\n";
    }

    for (int i = 0; i < bodyLen; ++i)
    {
        int step = pickStep(rng);
        int baseReg = baseRegs[pickBase(rng)];

        if (step < 35)
        {
            const LoadOp &op = kLoadOps[pickLoadOp(rng)];
            std::vector<int> offs = alignedOffsets(op.size);
            int off = offs[static_cast<size_t>(rng() % offs.size())];
            // rd is always a plain data register, never a base register: a
            // self-base-write (rd == baseReg) is already covered in isolation
            // by ReorderMemDeps.LoadWritesOwnBase_Edge. Letting it happen
            // here too would turn the base into an unpredictable value (the
            // loaded word), which can legitimately land near address 0 and
            // alias the program's own text_buffer -- the same class of
            // self-modifying-code hazard documented for the cost-model
            // generator, not a dep-graph correctness issue.
            int rd = kDataPool[pickData(rng)];
            out << op.mnemonic << " x" << rd << ", " << off << "(x" << baseReg << ")\n";
        }
        else if (step < 70)
        {
            const StoreOp &op = kStoreOps[pickStoreOp(rng)];
            std::vector<int> offs = alignedOffsets(op.size);
            int off = offs[static_cast<size_t>(rng() % offs.size())];
            int rs2 = kDataPool[pickData(rng)];
            out << op.mnemonic << " x" << rs2 << ", " << off << "(x" << baseReg << ")\n";
        }
        else if (step < 85)
        {
            out << "addi x" << baseReg << ", x0, " << kAddrPool[pickAddr(rng)] << "\n";
        }
        else
        {
            int rd = kDataPool[pickData(rng)];
            int rs1 = kDataPool[pickData(rng)];
            int rs2 = kDataPool[pickData(rng)];
            out << aluOps[pickAlu(rng)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

std::vector<uint64_t> memHeavyAddrs()
{
    std::vector<uint64_t> addrs;
    for (int base : kAddrPool)
    {
        for (int off : {0, 4, 8, 12})
        {
            addrs.push_back(static_cast<uint64_t>(base + off));
        }
    }
    return addrs;
}

// The same seed always yields the same programs, so the A/B report below
// runs on exactly the set the equivalence test uses.
std::vector<std::string> memHeavyPrograms(unsigned seed, int trials)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> lenDist(15, 30);
    std::vector<std::string> sources;
    for (int trial = 0; trial < trials; ++trial)
    {
        int len = lenDist(rng);
        sources.push_back(makeMemHeavyProgram(rng, len));
    }
    return sources;
}

template <typename VmType>
void randomizedMemDepsEquivalence(PipelineKind kind, unsigned seed, int trials, int &droppedEdges,
                                  long long &predictedBefore, long long &predictedAfter,
                                  long long &vmBefore, long long &vmAfter)
{
    std::vector<uint64_t> memAddrs = memHeavyAddrs();
    std::vector<std::string> sources = memHeavyPrograms(seed, trials);

    for (int trial = 0; trial < trials; ++trial)
    {
        const std::string &source = sources[static_cast<size_t>(trial)];
        SCOPED_TRACE(source);
        try
        {
            Kites::AssembledProgram program = assembleSource(source);
            checkEquivalence<VmType>("memdeps-random#" + std::to_string(trial), kind, program, memAddrs,
                                    droppedEdges, predictedBefore, predictedAfter, vmBefore, vmAfter);
        }
        catch (const std::exception &e)
        {
            FAIL() << "trial " << trial << " threw: " << e.what() << "\n" << source;
        }
    }
}

struct ExampleProgram
{
    std::string name;
    std::string source;
    std::vector<uint64_t> memAddrs;
};

// Same loop programs and baseline hazard examples as reorder_scheduler_test.cpp,
// reused here purely to report how this change affects their edge counts and
// stall numbers; their correctness is already asserted over there.
std::vector<ExampleProgram> loopExamples()
{
    return {
        {"loop_array_sum",
         "addi x20, x0, 1000\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "sw x21, 0(x20)\nsw x21, 4(x20)\nsw x21, 8(x20)\nsw x21, 12(x20)\n"
         "LOOP:\nbeq x23, x21, END\nlw x24, 0(x20)\nadd x22, x22, x24\n"
         "addi x20, x20, 4\naddi x23, x23, 1\njal x0, LOOP\nEND:\n",
         {1000, 1004, 1008, 1012}},
        {"loop_load_use_body",
         "addi x20, x0, 1100\naddi x21, x0, 3\naddi x22, x0, 0\n"
         "sw x21, 0(x20)\nsw x21, 4(x20)\nsw x21, 8(x20)\n"
         "LOOP2:\nbeq x22, x21, END2\nlw x24, 0(x20)\nadd x25, x24, x24\n"
         "addi x20, x20, 4\naddi x22, x22, 1\njal x0, LOOP2\nEND2:\n",
         {1100, 1104, 1108}},
        {"loop_store_then_load",
         "addi x20, x0, 1200\naddi x21, x0, 3\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "LOOP3:\nbeq x22, x21, END3\nsw x22, 0(x20)\nlw x24, 0(x20)\nadd x23, x23, x24\n"
         "addi x20, x20, 4\naddi x22, x22, 1\njal x0, LOOP3\nEND3:\n",
         {1200, 1204, 1208}},
    };
}

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
void reportExampleSet(const std::vector<ExampleProgram> &examples, const char *setName,
                      PipelineKind kind)
{
    int droppedEdges = 0;
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    for (const ExampleProgram &ex : examples)
    {
        Kites::AssembledProgram program = assembleSource(ex.source);
        checkEquivalence<VmType>(ex.name, kind, program, ex.memAddrs, droppedEdges, predictedBefore,
                                predictedAfter, vmBefore, vmAfter);
    }
    std::cout << "[MEMDEPS-REPORT] " << setName << " (" << pipelineName(kind)
              << ") droppable memory edges=" << droppedEdges
              << " | predicted stalls: " << predictedBefore << " -> " << predictedAfter
              << " | VM total_stalls_: " << vmBefore << " -> " << vmAfter << std::endl;
}

} // namespace

TEST(ReorderMemDeps, LoopExamplesReport_HF)
{
    reportExampleSet<Kites::RV5StageProcessorHF>(loopExamples(), "loop examples", PipelineKind::HF);
}

TEST(ReorderMemDeps, LoopExamplesReport_HNF)
{
    reportExampleSet<Kites::RV5StageProcessorHNF>(loopExamples(), "loop examples", PipelineKind::HNF);
}

TEST(ReorderMemDeps, BaselineExamplesReport_HF)
{
    reportExampleSet<Kites::RV5StageProcessorHF>(baselineExamples(), "baseline hazard examples",
                                                 PipelineKind::HF);
}

TEST(ReorderMemDeps, BaselineExamplesReport_HNF)
{
    reportExampleSet<Kites::RV5StageProcessorHNF>(baselineExamples(), "baseline hazard examples",
                                                  PipelineKind::HNF);
}

TEST(ReorderMemDeps, RandomizedMemHeavyEquivalence_HF)
{
    int droppedEdges = 0;
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    randomizedMemDepsEquivalence<Kites::RV5StageProcessorHF>(PipelineKind::HF, 24601u, 200,
                                                             droppedEdges, predictedBefore,
                                                             predictedAfter, vmBefore, vmAfter);
    std::cout << "[MEMDEPS-REPORT] randomized memory-heavy (h_f) n=200 droppable memory edges="
              << droppedEdges << " | predicted stalls: " << predictedBefore << " -> "
              << predictedAfter << " | VM total_stalls_: " << vmBefore << " -> " << vmAfter
              << std::endl;
}

TEST(ReorderMemDeps, RandomizedMemHeavyEquivalence_HNF)
{
    int droppedEdges = 0;
    long long predictedBefore = 0, predictedAfter = 0, vmBefore = 0, vmAfter = 0;
    randomizedMemDepsEquivalence<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, 86420u, 200,
                                                              droppedEdges, predictedBefore,
                                                              predictedAfter, vmBefore, vmAfter);
    std::cout << "[MEMDEPS-REPORT] randomized memory-heavy (h_nf) n=200 droppable memory edges="
              << droppedEdges << " | predicted stalls: " << predictedBefore << " -> "
              << predictedAfter << " | VM total_stalls_: " << vmBefore << " -> " << vmAfter
              << std::endl;
}

// =======================================================================
// A/B comparison: VM total_stalls_ for the original program, the program
// scheduled WITHOUT memory disambiguation, and scheduled WITH it, on the
// same programs. Both scheduled variants are also checked for equivalence
// against the original (GPRs, data memory, instructions_retired_).
// =======================================================================

namespace
{

// Written specifically to exercise base+offset disambiguation: typical
// array loops where a load of a DIFFERENT element follows a store off the
// same base pointer. Without disambiguation that load is pinned below the
// store; with it, the load can move up to fill a load-use slot.
std::vector<ExampleProgram> arrayLoopExamples()
{
    return {
        // In-place increment a[i] += 1, also summing the next element a[i+1].
        {"loop_inplace_inc_read_next",
         "addi x20, x0, 1500\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "addi x5, x0, 10\nsw x5, 0(x20)\naddi x5, x0, 20\nsw x5, 4(x20)\n"
         "addi x5, x0, 30\nsw x5, 8(x20)\naddi x5, x0, 40\nsw x5, 12(x20)\n"
         "addi x5, x0, 50\nsw x5, 16(x20)\n"
         "LOOPA:\n"
         "beq x22, x21, ENDA\n"
         "lw x24, 0(x20)\n"
         "addi x24, x24, 1\n"
         "sw x24, 0(x20)\n"
         "lw x25, 4(x20)\n"
         "add x23, x23, x25\n"
         "addi x20, x20, 4\n"
         "addi x22, x22, 1\n"
         "jal x0, LOOPA\n"
         "ENDA:\n",
         {1500, 1504, 1508, 1512, 1516}},
        // Array copy b[i] = a[i] with b = a + 32 bytes off the same pointer,
        // loading a[i+1] right after the store to b[i].
        {"loop_copy_load_next",
         "addi x20, x0, 1700\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "addi x5, x0, 7\nsw x5, 0(x20)\naddi x5, x0, 8\nsw x5, 4(x20)\n"
         "addi x5, x0, 9\nsw x5, 8(x20)\naddi x5, x0, 10\nsw x5, 12(x20)\n"
         "addi x5, x0, 11\nsw x5, 16(x20)\n"
         "LOOPB:\n"
         "beq x22, x21, ENDB\n"
         "lw x24, 0(x20)\n"
         "sw x24, 32(x20)\n"
         "lw x25, 4(x20)\n"
         "add x23, x23, x25\n"
         "addi x20, x20, 4\n"
         "addi x22, x22, 1\n"
         "jal x0, LOOPB\n"
         "ENDB:\n",
         {1700, 1704, 1708, 1712, 1716, 1732, 1736, 1740, 1744}},
    };
}

struct AbTotals
{
    long long original = 0;
    long long without = 0;
    long long with = 0;
};

template <typename VmType>
void abCompare(const std::string &name, PipelineKind kind, const Kites::AssembledProgram &program,
               const std::vector<uint64_t> &memAddrs, AbTotals &totals, bool printEach)
{
    RunResult original = runProgram<VmType>(program, memAddrs);
    unsigned scheduledStalls[2] = {0, 0};

    for (bool disamb : {false, true})
    {
        ScheduleReport report =
            scheduleProgram(program.text_buffer, kind, labelLeaders(program), disamb);
        Kites::AssembledProgram scheduled = program;
        scheduled.text_buffer = report.words;
        RunResult after = runProgram<VmType>(scheduled, memAddrs);

        const std::string tag = name + " (" + pipelineName(kind) + ", " +
                                (disamb ? "with" : "without") + " disambiguation)";
        EXPECT_EQ(original.gprs, after.gprs) << tag << ": GPRs differ";
        EXPECT_EQ(original.memWords, after.memWords) << tag << ": data memory differs";
        EXPECT_EQ(original.instructionsRetired, after.instructionsRetired)
            << tag << ": instructions_retired_ differs";
        EXPECT_LE(after.totalStalls, original.totalStalls) << tag << ": worse than original";
        scheduledStalls[disamb ? 1 : 0] = after.totalStalls;
    }

    totals.original += original.totalStalls;
    totals.without += scheduledStalls[0];
    totals.with += scheduledStalls[1];

    if (printEach)
    {
        std::cout << "[AB-REPORT] " << name << " (" << pipelineName(kind)
                  << ") VM total_stalls_: original=" << original.totalStalls
                  << " without=" << scheduledStalls[0] << " with=" << scheduledStalls[1]
                  << std::endl;
    }
}

template <typename VmType>
void abExampleSet(const std::vector<ExampleProgram> &examples, const char *setName, PipelineKind kind)
{
    AbTotals totals;
    for (const ExampleProgram &ex : examples)
    {
        abCompare<VmType>(ex.name, kind, assembleSource(ex.source), ex.memAddrs, totals, true);
    }
    std::cout << "[AB-REPORT] " << setName << " total (" << pipelineName(kind)
              << ") original=" << totals.original << " without=" << totals.without
              << " with=" << totals.with << std::endl;
}

template <typename VmType>
void abRandomSet(PipelineKind kind, unsigned seed)
{
    AbTotals totals;
    std::vector<uint64_t> memAddrs = memHeavyAddrs();
    std::vector<std::string> sources = memHeavyPrograms(seed, 200);
    for (size_t t = 0; t < sources.size(); ++t)
    {
        SCOPED_TRACE(sources[t]);
        abCompare<VmType>("memdeps-random#" + std::to_string(t), kind, assembleSource(sources[t]),
                          memAddrs, totals, false);
    }
    std::cout << "[AB-REPORT] randomized memory-heavy n=200 total (" << pipelineName(kind)
              << ") original=" << totals.original << " without=" << totals.without
              << " with=" << totals.with << std::endl;
}

} // namespace

TEST(ReorderMemDeps, AB_LoopExamples_HF)
{
    abExampleSet<Kites::RV5StageProcessorHF>(loopExamples(), "existing loops", PipelineKind::HF);
}

TEST(ReorderMemDeps, AB_LoopExamples_HNF)
{
    abExampleSet<Kites::RV5StageProcessorHNF>(loopExamples(), "existing loops", PipelineKind::HNF);
}

TEST(ReorderMemDeps, AB_RandomMemHeavy_HF)
{
    abRandomSet<Kites::RV5StageProcessorHF>(PipelineKind::HF, 24601u);
}

TEST(ReorderMemDeps, AB_RandomMemHeavy_HNF)
{
    abRandomSet<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, 86420u);
}

TEST(ReorderMemDeps, AB_ArrayLoops_HF)
{
    abExampleSet<Kites::RV5StageProcessorHF>(arrayLoopExamples(), "array loops", PipelineKind::HF);
}

TEST(ReorderMemDeps, AB_ArrayLoops_HNF)
{
    abExampleSet<Kites::RV5StageProcessorHNF>(arrayLoopExamples(), "array loops", PipelineKind::HNF);
}
