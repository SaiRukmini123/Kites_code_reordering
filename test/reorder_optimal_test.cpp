#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "utils/utils.h"

using namespace Kites::reorder;
namespace fs = std::filesystem;

namespace
{

const fs::path kRepoRoot = fs::path(__FILE__).parent_path().parent_path();

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
struct NamedProgram
{
    std::string name;
    std::string source;
};

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

const char *pipelineName(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

std::vector<DecodedInst> decodeBlock(const std::vector<uint32_t> &words, const Block &block)
{
    std::vector<DecodedInst> insts;
    for (size_t k = block.begin; k < block.end; ++k)
    {
        insts.push_back(decode(words[k]));
    }
    return insts;
}

std::vector<DecodedInst> inOrder(const std::vector<DecodedInst> &insts, const std::vector<size_t> &order)
{
    std::vector<DecodedInst> out;
    for (size_t i : order)
    {
        out.push_back(insts[i]);
    }
    return out;
}

// The two instructions just before the block in the word array (as
// scheduleProgram would see them for an unscheduled predecessor).
struct Context
{
    DecodedInst prev1{}, prev2{};
    HeadContext ctx;
};

void fillContext(Context &c, const std::vector<uint32_t> &words, const Block &block)
{
    c.ctx = HeadContext{};
    if (block.begin >= 1)
    {
        c.prev1 = decode(words[block.begin - 1]);
        c.ctx.prev1 = &c.prev1;
    }
    if (block.begin >= 2)
    {
        c.prev2 = decode(words[block.begin - 2]);
        c.ctx.prev2 = &c.prev2;
    }
}

bool isPermutationRespecting(const std::vector<size_t> &order, const DepGraph &g)
{
    if (order.size() != g.size)
    {
        return false;
    }
    std::vector<size_t> position(order.size(), order.size());
    for (size_t pos = 0; pos < order.size(); ++pos)
    {
        if (order[pos] >= order.size() || position[order[pos]] != order.size())
        {
            return false;
        }
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

// Every topological order of g, the minimum predicted stalls among them.
int naiveMinimum(const std::vector<DecodedInst> &insts, const DepGraph &g, PipelineKind kind,
                 const HeadContext &ctx, std::vector<size_t> *bestOrder = nullptr)
{
    const size_t n = insts.size();
    std::vector<size_t> remaining(n);
    for (size_t j = 0; j < n; ++j)
    {
        remaining[j] = g.pred[j].size();
    }
    std::vector<bool> placed(n, false);
    std::vector<size_t> order;
    int best = 1 << 30;

    std::function<void()> enumerate = [&]()
    {
        if (order.size() == n)
        {
            const int stalls = predictStalls(inOrder(insts, order), kind, ctx);
            if (stalls < best)
            {
                best = stalls;
                if (bestOrder != nullptr)
                {
                    *bestOrder = order;
                }
            }
            return;
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (placed[i] || remaining[i] != 0)
            {
                continue;
            }
            placed[i] = true;
            order.push_back(i);
            for (size_t s : g.succ[i]) --remaining[s];
            enumerate();
            for (size_t s : g.succ[i]) ++remaining[s];
            order.pop_back();
            placed[i] = false;
        }
    };
    enumerate();
    return best;
}

// ---------------------------------------------------------------------
// Random program generators (same shapes and safe data bases as the
// scheduler, memory-dependency and assembly-writer tests).
// ---------------------------------------------------------------------
std::string makeStraightLine(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "sll", "srl", "sra", "mul", "div", "rem"};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    std::ostringstream out;
    for (int i = 0; i < 3; ++i)
    {
        out << "addi x" << baseReg[i] << ", x0, " << baseAddr[i] << "\n";
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(14);
        const int rd = pool[pick(13)], rs1 = pool[pick(13)], rs2 = pool[pick(13)];
        if (op == 0)
        {
            int imm = pick(2048);
            if (pick(2) == 1)
            {
                imm = (imm & ~0x1F) | pool[pick(13)];   // immediate bits collide with a live register
            }
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else if (op == 12)
        {
            out << "lw x" << rd << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else if (op == 13)
        {
            out << "sw x" << rs2 << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else
        {
            out << rOps[op - 1] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

std::string makeBranchy(std::mt19937 &rng, int bodyLen)
{
    static const char *branchOps[] = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    std::vector<int> labelPos;
    for (int k = 0, count = 1 + pick(3); k < count; ++k)
    {
        labelPos.push_back(1 + pick(bodyLen));
    }
    std::sort(labelPos.begin(), labelPos.end());

    // Straight-line body from makeStraightLine, one instruction per line,
    // with forward-only branches and jals mixed in.
    std::istringstream body(makeStraightLine(rng, bodyLen));
    std::vector<std::string> lines;
    for (std::string line; std::getline(body, line);)
    {
        lines.push_back(line);
    }

    std::ostringstream out;
    for (int i = 0; i < 3; ++i)
    {
        out << lines[static_cast<size_t>(i)] << "\n";
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        for (size_t k = 0; k < labelPos.size(); ++k)
        {
            if (labelPos[k] == i)
            {
                out << "skip" << k << ":\n";
            }
        }
        std::vector<size_t> forward;
        for (size_t k = 0; k < labelPos.size(); ++k)
        {
            if (labelPos[k] > i)
            {
                forward.push_back(k);
            }
        }
        if (!forward.empty() && pick(6) == 0)
        {
            const size_t k = forward[static_cast<size_t>(pick(static_cast<int>(forward.size())))];
            if (pick(3) == 0)
            {
                out << "jal x0, skip" << k << "\n";
            }
            else
            {
                out << branchOps[pick(6)] << " x" << (5 + pick(4)) << ", x" << (13 + pick(4)) << ", skip" << k
                    << "\n";
            }
        }
        else
        {
            out << lines[static_cast<size_t>(3 + i)] << "\n";
        }
    }
    for (size_t k = 0; k < labelPos.size(); ++k)
    {
        if (labelPos[k] == bodyLen)
        {
            out << "skip" << k << ":\n";
        }
    }
    return out.str();
}

std::string makeMemHeavy(std::mt19937 &rng, int bodyLen)
{
    struct Op
    {
        const char *mnemonic;
        int size;
    };
    static const Op loads[] = {{"lb", 1}, {"lh", 2}, {"lw", 4}, {"ld", 8}, {"lbu", 1}, {"lhu", 2}, {"lwu", 4}};
    static const Op stores[] = {{"sb", 1}, {"sh", 2}, {"sw", 4}, {"sd", 8}};
    static const int addrPool[] = {1000, 1100, 1200, 1300};
    static const int dataPool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    auto offset = [&](int size) { return size * pick(16 / size); };

    const std::vector<int> bases = pick(2) == 1 ? std::vector<int>{10, 11} : std::vector<int>{10};
    std::ostringstream out;
    for (int b : bases)
    {
        out << "addi x" << b << ", x0, " << addrPool[pick(4)] << "\n";
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        const int step = pick(100);
        const int base = bases[static_cast<size_t>(pick(static_cast<int>(bases.size())))];
        if (step < 35)
        {
            const Op &op = loads[pick(7)];
            out << op.mnemonic << " x" << dataPool[pick(12)] << ", " << offset(op.size) << "(x" << base << ")\n";
        }
        else if (step < 70)
        {
            const Op &op = stores[pick(4)];
            out << op.mnemonic << " x" << dataPool[pick(12)] << ", " << offset(op.size) << "(x" << base << ")\n";
        }
        else if (step < 85)
        {
            out << "addi x" << base << ", x0, " << addrPool[pick(4)] << "\n";
        }
        else
        {
            out << "add x" << dataPool[pick(12)] << ", x" << dataPool[pick(12)] << ", x" << dataPool[pick(12)]
                << "\n";
        }
    }
    return out.str();
}

std::vector<NamedProgram> randomSet(const char *prefix, std::string (*make)(std::mt19937 &, int),
                                    unsigned seed, int count, int minLen, int maxLen)
{
    std::mt19937 rng(seed);
    std::vector<NamedProgram> programs;
    for (int t = 0; t < count; ++t)
    {
        const int len = std::uniform_int_distribution<int>(minLen, maxLen)(rng);
        programs.push_back({std::string(prefix) + "#" + std::to_string(t), make(rng, len)});
    }
    return programs;
}

std::vector<NamedProgram> programsIn(const std::vector<fs::path> &dirs)
{
    std::vector<NamedProgram> programs;
    for (const fs::path &dir : dirs)
    {
        for (const auto &entry : fs::directory_iterator(dir))
        {
            const std::string ext = entry.path().extension().string();
            if (entry.is_regular_file() && (ext == ".s" || ext == ".asm"))
            {
                programs.push_back({entry.path().filename().string(), readFile(entry.path())});
            }
        }
    }
    std::sort(programs.begin(), programs.end(),
              [](const NamedProgram &a, const NamedProgram &b) { return a.name < b.name; });
    return programs;
}

std::vector<NamedProgram> examplePrograms()
{
    return programsIn({kRepoRoot / "examples", kRepoRoot / "examples" / "fd-extension"});
}

std::vector<NamedProgram> demoPrograms() { return programsIn({kRepoRoot / "test" / "reorder_demo"}); }

struct ProgramSet
{
    const char *name;
    std::vector<NamedProgram> programs;
};

std::vector<ProgramSet> allProgramSets()
{
    return {
        {"examples", examplePrograms()},
        {"demo", demoPrograms()},
        {"random straight-line", randomSet("straight", makeStraightLine, 13579u, 100, 10, 20)},
        {"random branchy", randomSet("branchy", makeBranchy, 777u, 100, 10, 25)},
        {"random memory-heavy", randomSet("memheavy", makeMemHeavy, 24601u, 100, 15, 30)},
    };
}

void printSummary(const std::string &label, const OptimalSummary &s, int greedyModeTotal)
{
    std::cout << "[OPTIMAL] " << label << ": blocks=" << s.blocks << " greedy=" << s.greedyStalls
              << " optimal=" << s.optimalStalls << " gap=" << s.gap()
              << " | greedy already optimal in " << s.greedyAlreadyOptimal << "/" << s.provenBlocks
              << " proven blocks, improved=" << s.improvedBlocks << ", best-found=" << s.bestFoundBlocks
              << ", cap-hit=" << s.capHitBlocks << ", too-large=" << s.tooLargeBlocks
              << " | greedy-mode program total=" << greedyModeTotal << " | nodes=" << s.nodesSearched
              << std::endl;
}

void addInto(OptimalSummary &total, const OptimalSummary &s)
{
    total.greedyStalls += s.greedyStalls;
    total.optimalStalls += s.optimalStalls;
    total.blocks += s.blocks;
    total.provenBlocks += s.provenBlocks;
    total.greedyAlreadyOptimal += s.greedyAlreadyOptimal;
    total.improvedBlocks += s.improvedBlocks;
    total.bestFoundBlocks += s.bestFoundBlocks;
    total.capHitBlocks += s.capHitBlocks;
    total.tooLargeBlocks += s.tooLargeBlocks;
    total.nodesSearched += s.nodesSearched;
}

} // namespace

// =======================================================================
// The memo key drops fields checkHazard() never reads; that must never
// change the stalls of any continuation.
// =======================================================================

TEST(ReorderOptimal, CanonicalWindowKeepsFutureStalls)
{
    std::mt19937 rng(31337u);
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    auto randomProducer = [&]()
    {
        if (pick(4) == 0)
        {
            return Producer{};
        }
        return Producer{true, static_cast<uint8_t>(1 + pick(20)), pick(2) == 1};
    };

    int checked = 0;
    for (int t = 0; t < 300; ++t)
    {
        const Kites::AssembledProgram program = assembleSource(makeStraightLine(rng, 8));
        std::vector<DecodedInst> suffix;
        for (uint32_t w : program.text_buffer)
        {
            suffix.push_back(decode(w));
        }
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            for (int s = 0; s < 10; ++s)
            {
                const WindowState w{randomProducer(), randomProducer()};
                WindowState a = w;
                WindowState b = canonicalWindow(w, kind);
                for (const DecodedInst &d : suffix)
                {
                    ASSERT_EQ(issue(a, d, kind), issue(b, d, kind)) << pipelineName(kind) << " trial " << t;
                }
                ++checked;
            }
        }
    }
    std::cout << "[OPTIMAL] canonical window checked on " << checked << " window/suffix pairs" << std::endl;
}

// =======================================================================
// (1) Blocks of 7 or fewer instructions: the optimum equals a naive
// enumeration of every legal order.
// =======================================================================

TEST(ReorderOptimal, MatchesNaiveEnumeration_UpTo7)
{
    int blocks = 0, greedyWorse = 0;
    auto check = [&](const std::vector<uint32_t> &words, const Block &block, PipelineKind kind,
                     const HeadContext &ctx, const std::string &name)
    {
        const std::vector<DecodedInst> insts = decodeBlock(words, block);
        const DepGraph g = buildDepGraph(words, block);
        const int naive = naiveMinimum(insts, g, kind, ctx);
        const OptimalBlockResult r = scheduleBlockOptimalDetailed(words, block, kind, ctx);
        EXPECT_TRUE(r.provenOptimal()) << name;
        EXPECT_EQ(r.optimalStalls, naive) << name << " (" << pipelineName(kind) << ")";
        EXPECT_EQ(predictStalls(inOrder(insts, r.order), kind, ctx), r.optimalStalls) << name;
        EXPECT_TRUE(isPermutationRespecting(r.order, g)) << name;
        ++blocks;
        greedyWorse += (r.greedyStalls > naive) ? 1 : 0;
    };

    // Random blocks of 2..7 instructions, with the two instructions before
    // them as head context, and again with no context.
    std::mt19937 rng(4711u);
    for (int t = 0; t < 400; ++t)
    {
        const int k = 2 + t % 6;
        const Kites::AssembledProgram program = assembleSource(makeStraightLine(rng, k + 2));
        const size_t n = program.text_buffer.size();
        const Block block{n - static_cast<size_t>(k), n, false};
        Context c;
        fillContext(c, program.text_buffer, block);
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            check(program.text_buffer, block, kind, c.ctx, "random#" + std::to_string(t));
            check(program.text_buffer, block, kind, HeadContext{}, "random#" + std::to_string(t) + " no ctx");
        }
    }

    // Every block of at most 7 instructions in the example and demo programs.
    for (const std::vector<NamedProgram> &set : {examplePrograms(), demoPrograms()})
    {
        for (const NamedProgram &p : set)
        {
            const Kites::AssembledProgram program = assembleSource(p.source);
            for (const Block &block : findBlocks(program.text_buffer, labelLeaders(program)))
            {
                if (block.end - block.begin > 7)
                {
                    continue;
                }
                Context c;
                fillContext(c, program.text_buffer, block);
                for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
                {
                    check(program.text_buffer, block, kind, c.ctx, p.name);
                }
            }
        }
    }
    std::cout << "[OPTIMAL] naive enumeration agreed on " << blocks << " blocks of <= 7 instructions; greedy "
              << "was worse than the optimum on " << greedyWorse << " of them" << std::endl;
}

// =======================================================================
// (2) Optimal <= greedy on every block of the example, demo and random
// programs, plus the greedy-vs-optimal report.
// =======================================================================

TEST(ReorderOptimal, NeverWorseThanGreedy_AllBlocks)
{
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        for (const ProgramSet &set : allProgramSets())
        {
            OptimalSummary total;
            int greedyModeTotal = 0;
            for (const NamedProgram &p : set.programs)
            {
                const Kites::AssembledProgram program = assembleSource(p.source);
                const std::vector<size_t> leaders = labelLeaders(program);
                const ScheduleReport optimal =
                    scheduleProgram(program.text_buffer, kind, leaders, true, ScheduleMode::Optimal);
                const ScheduleReport greedy = scheduleProgram(program.text_buffer, kind, leaders);
                for (const BlockStalls &b : optimal.blocks)
                {
                    EXPECT_LE(b.stallsAfter, b.greedyStalls) << p.name << " block at " << b.block.begin;
                    EXPECT_EQ(b.stallsAfter, b.optimalStalls) << p.name;
                }
                const OptimalSummary s = summarizeOptimal(optimal);
                addInto(total, s);
                greedyModeTotal += greedy.stallsAfter;
                if (set.programs.size() <= 10)
                {
                    printSummary(std::string(set.name) + " " + p.name + " (" + pipelineName(kind) + ")", s,
                                 greedy.stallsAfter);
                }
            }
            printSummary(std::string(set.name) + " total (" + pipelineName(kind) + ")", total, greedyModeTotal);
        }
    }
}

// =======================================================================
// (3) Every optimal order respects the dependency graph.
// =======================================================================

TEST(ReorderOptimal, OrdersRespectDependencyGraph)
{
    int blocks = 0;
    for (const ProgramSet &set : allProgramSets())
    {
        for (const NamedProgram &p : set.programs)
        {
            const Kites::AssembledProgram program = assembleSource(p.source);
            for (const Block &block : findBlocks(program.text_buffer, labelLeaders(program)))
            {
                Context c;
                fillContext(c, program.text_buffer, block);
                const DepGraph g = buildDepGraph(program.text_buffer, block);
                for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
                {
                    const OptimalBlockResult r = scheduleBlockOptimalDetailed(program.text_buffer, block, kind, c.ctx);
                    ASSERT_TRUE(isPermutationRespecting(r.order, g)) << p.name << " block at " << block.begin;
                    EXPECT_LE(r.optimalStalls, r.greedyStalls) << p.name;
                    EXPECT_LE(r.optimalStalls, r.originalStalls) << p.name;
                    ++blocks;
                }
            }
        }
    }
    std::cout << "[OPTIMAL] dependency graph respected on " << blocks << " block schedules" << std::endl;
}

// =======================================================================
// (4) VM equivalence of optimal schedules: demo programs and 100 random
// programs per pipeline. Same final registers, memory and retired count as
// the original, and real stalls never above the original.
// =======================================================================

namespace
{

struct VmOutcome
{
    unsigned stalls = 0;
    unsigned retired = 0;
    std::vector<uint64_t> gprs;
    std::vector<uint32_t> memory;   // every word from the end of the text up to 0x2000
};

template <typename VmType>
VmOutcome runOnVm(const Kites::AssembledProgram &program)
{
    Kites::setupVmStateDirectory();
    Kites::reorder_detail::CoutSilencer quiet;
    auto vm = std::make_unique<VmType>();
    vm->LoadProgram(program);
    vm->DebugRun();
    VmOutcome o;
    o.stalls = vm->total_stalls_;
    o.retired = vm->instructions_retired_;
    for (int r = 1; r <= 31; ++r)
    {
        o.gprs.push_back(vm->registers_.ReadGpr(r));
    }
    for (uint64_t a = program.text_buffer.size() * 4; a < 0x2000; a += 4)
    {
        o.memory.push_back(vm->memory_controller_.readWord(a));
    }
    return o;
}

struct VmTotals
{
    long long original = 0, greedy = 0, optimal = 0;
};

template <typename VmType>
void vmCheck(const NamedProgram &p, PipelineKind kind, VmTotals &totals, bool print)
{
    const Kites::AssembledProgram program = assembleSource(p.source);
    const std::vector<size_t> leaders = labelLeaders(program);
    const ScheduleReport greedy = scheduleProgram(program.text_buffer, kind, leaders);
    const ScheduleReport optimal = scheduleProgram(program.text_buffer, kind, leaders, true, ScheduleMode::Optimal);

    const VmOutcome a = runOnVm<VmType>(program);
    const VmOutcome g = runOnVm<VmType>(Kites::withText(program, greedy.words));
    const VmOutcome o = runOnVm<VmType>(Kites::withText(program, optimal.words));

    const std::string tag = p.name + " (" + pipelineName(kind) + ")";
    EXPECT_EQ(a.gprs, o.gprs) << tag << ": GPRs differ";
    EXPECT_EQ(a.memory, o.memory) << tag << ": memory differs";
    EXPECT_EQ(a.retired, o.retired) << tag << ": instructions_retired_ differs";
    EXPECT_LE(o.stalls, a.stalls) << tag << ": optimal schedule stalls more than the original";

    totals.original += a.stalls;
    totals.greedy += g.stalls;
    totals.optimal += o.stalls;
    if (print)
    {
        std::cout << "[OPTIMAL-VM] " << tag << " predicted greedy " << greedy.stallsAfter << " / optimal "
                  << optimal.stallsAfter << " | VM total_stalls_ original " << a.stalls << ", greedy "
                  << g.stalls << ", optimal " << o.stalls << std::endl;
    }
}

template <typename VmType>
void vmEquivalence(PipelineKind kind, unsigned seed)
{
    VmTotals demo;
    for (const NamedProgram &p : demoPrograms())
    {
        vmCheck<VmType>(p, kind, demo, true);
    }
    VmTotals random;
    for (const NamedProgram &p : randomSet("straight", makeStraightLine, seed, 100, 10, 20))
    {
        vmCheck<VmType>(p, kind, random, false);
    }
    std::cout << "[OPTIMAL-VM] demo total (" << pipelineName(kind) << ") original " << demo.original
              << ", greedy " << demo.greedy << ", optimal " << demo.optimal << std::endl;
    std::cout << "[OPTIMAL-VM] 100 random straight-line (" << pipelineName(kind) << ") original "
              << random.original << ", greedy " << random.greedy << ", optimal " << random.optimal << std::endl;
}

} // namespace

TEST(ReorderOptimal, VmEquivalence_HF) { vmEquivalence<Kites::RV5StageProcessorHF>(PipelineKind::HF, 13579u); }

TEST(ReorderOptimal, VmEquivalence_HNF) { vmEquivalence<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, 97531u); }

// =======================================================================
// (5) Cases where greedy is provably suboptimal: a seeded search over small
// random blocks finds the smallest one per pipeline.
// =======================================================================

TEST(ReorderOptimal, SearchFindsGreedySuboptimalCases)
{
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        std::mt19937 rng(kind == PipelineKind::HF ? 100u : 200u);
        bool found = false;
        for (int k = 2; k <= 6 && !found; ++k)
        {
            for (int t = 0; t < 3000 && !found; ++t)
            {
                const std::string source = makeStraightLine(rng, k);
                const Kites::AssembledProgram program = assembleSource(source);
                const size_t n = program.text_buffer.size();
                const Block block{n - static_cast<size_t>(k), n, false};
                Context c;
                fillContext(c, program.text_buffer, block);
                const std::vector<DecodedInst> insts = decodeBlock(program.text_buffer, block);
                const std::vector<size_t> greedyOrder = scheduleBlock(program.text_buffer, block, kind, c.ctx);
                const int greedy = predictStalls(inOrder(insts, greedyOrder), kind, c.ctx);
                std::vector<size_t> bestOrder;
                const int best = naiveMinimum(insts, buildDepGraph(program.text_buffer, block), kind, c.ctx,
                                              &bestOrder);
                if (best < greedy)
                {
                    found = true;
                    std::cout << "[OPTIMAL] smallest greedy-suboptimal block found (" << pipelineName(kind)
                              << "): " << k << " instructions, greedy " << greedy << ", optimum " << best
                              << "\n" << source << "greedy order:";
                    for (size_t i : greedyOrder) std::cout << " " << i;
                    std::cout << "\noptimal order:";
                    for (size_t i : bestOrder) std::cout << " " << i;
                    std::cout << std::endl;
                }
            }
        }
        EXPECT_TRUE(found) << pipelineName(kind);
    }
}

// The two smallest cases that search found (seeds 100 / 200 above), kept as
// regressions. Both blocks have 2 independent instructions; the head context
// is the two addis before them. In both, the second instruction's immediate
// bits name the first one's destination, so the hazard unit sees a false
// dependency (the quirk) -- but only one step later. Greedy looks one
// instruction ahead: both candidates cost 0 when placed first, so the tie
// break decides, and it picks the order that pays the false stall.
namespace
{

void expectGreedySuboptimal(const std::string &source, PipelineKind kind, int greedyStalls, int optimum,
                            const std::vector<size_t> &optimalOrder)
{
    const Kites::AssembledProgram program = assembleSource(source);
    const std::vector<uint32_t> &words = program.text_buffer;
    const Block block{words.size() - 2, words.size(), false};
    Context c;
    fillContext(c, words, block);

    const OptimalBlockResult r = scheduleBlockOptimalDetailed(words, block, kind, c.ctx);
    EXPECT_EQ(r.greedyStalls, greedyStalls);
    EXPECT_EQ(r.optimalStalls, optimum);
    EXPECT_EQ(r.method, OptimizerMethod::Optimal);
    EXPECT_EQ(r.order, optimalOrder);
    // "Provably": the naive enumeration of every legal order agrees.
    EXPECT_EQ(naiveMinimum(decodeBlock(words, block), buildDepGraph(words, block), kind, c.ctx), optimum);
}

} // namespace

TEST(ReorderOptimal, Regression_GreedySuboptimal_HF)
{
    // 1550 = 0b110000_01110: rs2 bits of the addi are 14, the lw's rd. Both
    // cost 0 first; the tie-break prefers the load, so the addi lands right
    // after it and takes a false load-use stall. Optimal: addi first.
    expectGreedySuboptimal("addi x10, x0, 1000\naddi x11, x0, 1200\naddi x12, x0, 1400\n"
                           "lw x14, 0(x12)\naddi x8, x8, 1550\n",
                           PipelineKind::HF, 1, 0, {1, 0});
}

TEST(ReorderOptimal, Regression_GreedySuboptimal_HNF)
{
    // 905 = 0b11100_01001: rs2 bits of the addi are 9, the srl's rd. Both
    // cost 0 first; the tie goes to original order (srl first), so the addi
    // sits at distance 1 from x9's producer: 2 false stalls. Optimal: addi first.
    expectGreedySuboptimal("addi x10, x0, 1000\naddi x11, x0, 1200\naddi x12, x0, 1400\n"
                           "srl x9, x18, x14\naddi x7, x6, 905\n",
                           PipelineKind::HNF, 2, 0, {1, 0});
}

// =======================================================================
// (6) Runtime. An adversarial 64-instruction block (the largest size the
// search accepts): 30 independent leaves feeding a reduction tree, a short
// chain, then lw x31 and a branch on x31. The lw depends on everything, so
// it is always second to last and the load-use stall at the end can't be
// avoided; before that nothing stalls on rv5-h-f, so no partial order can
// be cut early and the number of positions is astronomically large. The
// node cap must stop it, and the time must stay bounded.
// =======================================================================

namespace
{

uint32_t encAddi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t encAdd(int rd, int rs1, int rs2) { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33; }
uint32_t encLw(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03; }
uint32_t encBeq(int rs1, int rs2) { return (rs2 << 20) | (rs1 << 15) | (4 << 8) | 0x63; }   // offset +8

std::vector<uint32_t> adversarialBlock()
{
    std::vector<uint32_t> w;
    for (int r = 1; r <= 30; ++r)
    {
        w.push_back(encAddi(r, 0, r));
    }
    std::vector<int> live;
    for (int r = 1; r <= 30; ++r)
    {
        live.push_back(r);
    }
    while (live.size() > 1)   // 29 adds
    {
        std::vector<int> next;
        for (size_t k = 0; k + 1 < live.size(); k += 2)
        {
            w.push_back(encAdd(live[k], live[k], live[k + 1]));
            next.push_back(live[k]);
        }
        if (live.size() % 2 == 1)
        {
            next.push_back(live.back());
        }
        live = next;
    }
    for (int k = 0; k < 3; ++k)
    {
        w.push_back(encAddi(live[0], live[0], 1));
    }
    w.push_back(encLw(31, live[0], 0));
    w.push_back(encBeq(31, 0));
    return w;
}

} // namespace

TEST(ReorderOptimal, RuntimeBound_AdversarialLargestBlock)
{
    const std::vector<uint32_t> words = adversarialBlock();
    ASSERT_EQ(words.size(), kMaxOptimalBlockSize);
    const Block block{0, words.size(), true};
    const DepGraph g = buildDepGraph(words, block);

    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const auto start = std::chrono::steady_clock::now();
        const OptimalBlockResult r = scheduleBlockOptimalDetailed(words, block, kind);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        std::cout << "[OPTIMAL-TIME] adversarial 64-instruction block (" << pipelineName(kind)
                  << "): " << seconds << " s, " << r.nodesSearched << " nodes, greedy " << r.greedyStalls
                  << ", returned " << r.optimalStalls << ", method: " << optimizerMethodName(r.method) << std::endl;
        EXPECT_TRUE(isPermutationRespecting(r.order, g));
        EXPECT_LE(r.optimalStalls, r.greedyStalls);
        EXPECT_LE(r.nodesSearched, kDefaultOptimalNodeBudget + 1);
        EXPECT_LT(seconds, 5.0);
        if (kind == PipelineKind::HF)
        {
            EXPECT_EQ(r.method, OptimizerMethod::GreedyCapHit);   // greedy's 1 stall is optimal, but unprovable here
        }
    }
}

// When the cap is hit, the best order found so far is returned if it beats
// greedy ("best found, not proven optimal"); otherwise greedy's order.
// Forced here with small caps on blocks where the full search beats greedy.
TEST(ReorderOptimal, CapHit_ReturnsBestFoundOrGreedy)
{
    int bestFound = 0, capHit = 0;
    for (const NamedProgram &p : randomSet("memheavy", makeMemHeavy, 24601u, 100, 15, 30))
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const std::vector<uint32_t> &words = program.text_buffer;
        for (const Block &block : findBlocks(words, labelLeaders(program)))
        {
            Context c;
            fillContext(c, words, block);
            const OptimalBlockResult full = scheduleBlockOptimalDetailed(words, block, PipelineKind::HNF, c.ctx);
            if (full.method != OptimizerMethod::Optimal || full.optimalStalls == full.greedyStalls ||
                full.nodesSearched < 4)
            {
                continue;
            }
            const DepGraph g = buildDepGraph(words, block);
            const std::vector<DecodedInst> insts = decodeBlock(words, block);
            for (uint64_t cap : {uint64_t{1}, full.nodesSearched / 4, full.nodesSearched / 2})
            {
                const OptimalBlockResult r =
                    scheduleBlockOptimalDetailed(words, block, PipelineKind::HNF, c.ctx, true, cap);
                if (r.provenOptimal())
                {
                    continue;   // a smaller search can still finish (the memo differs)
                }
                EXPECT_TRUE(isPermutationRespecting(r.order, g)) << p.name;
                EXPECT_EQ(predictStalls(inOrder(insts, r.order), PipelineKind::HNF, c.ctx), r.optimalStalls);
                EXPECT_GE(r.optimalStalls, full.optimalStalls) << p.name;
                if (r.optimalStalls < r.greedyStalls)
                {
                    EXPECT_EQ(r.method, OptimizerMethod::BestFound) << p.name;
                    ++bestFound;
                }
                else
                {
                    EXPECT_EQ(r.method, OptimizerMethod::GreedyCapHit) << p.name;
                    EXPECT_EQ(r.optimalStalls, r.greedyStalls) << p.name;
                    ++capHit;
                }
            }
        }
    }
    std::cout << "[OPTIMAL] forced caps: " << bestFound << " best-found (beat greedy, not proven), " << capHit
              << " greedy-on-cap" << std::endl;
    EXPECT_GT(bestFound, 0);
    EXPECT_GT(capHit, 0);
}

TEST(ReorderOptimal, Timing_LargestDemoBlock)
{
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        std::string largestName;
        size_t largestSize = 0;
        double largestSeconds = 0, slowestSeconds = 0;
        uint64_t largestNodes = 0;
        for (const NamedProgram &p : demoPrograms())
        {
            const Kites::AssembledProgram program = assembleSource(p.source);
            for (const Block &block : findBlocks(program.text_buffer, labelLeaders(program)))
            {
                Context c;
                fillContext(c, program.text_buffer, block);
                const auto start = std::chrono::steady_clock::now();
                const OptimalBlockResult r = scheduleBlockOptimalDetailed(program.text_buffer, block, kind, c.ctx);
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                slowestSeconds = std::max(slowestSeconds, seconds);
                if (block.end - block.begin > largestSize)
                {
                    largestSize = block.end - block.begin;
                    largestName = p.name;
                    largestSeconds = seconds;
                    largestNodes = r.nodesSearched;
                }
            }
        }
        std::cout << "[OPTIMAL-TIME] largest demo block (" << pipelineName(kind) << "): " << largestSize
                  << " instructions in " << largestName << ", " << largestSeconds * 1e3 << " ms, " << largestNodes
                  << " nodes; slowest demo block " << slowestSeconds * 1e3 << " ms" << std::endl;
        EXPECT_LT(slowestSeconds, 1.0);
    }
}
