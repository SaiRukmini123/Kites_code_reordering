#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/asm_writer.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder_la_programs.h"
#include "utils/utils.h"

using namespace Kites::reorder;
namespace fs = std::filesystem;

namespace
{

const fs::path kRepoRoot = fs::path(__FILE__).parent_path().parent_path();
constexpr unsigned kCycleCap = 100000;

using NamedProgram = reorder_test::LaProgram;
using reorder_test::laPairs;
using reorder_test::laPrograms;

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

const char *pipelineName(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

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
                std::ifstream in(entry.path());
                std::ostringstream text;
                text << in.rdbuf();
                programs.push_back({entry.path().filename().string(), text.str()});
            }
        }
    }
    std::sort(programs.begin(), programs.end(),
              [](const NamedProgram &a, const NamedProgram &b) { return a.name < b.name; });
    return programs;
}

// Stalls charged to each instruction of a straight-line word stream, as
// differences of predictStalls over growing prefixes (exact for the VM on
// straight-line code).
std::vector<int> stallsPerWord(const std::vector<uint32_t> &words, PipelineKind kind)
{
    std::vector<int> per(words.size(), 0);
    WindowState w;
    for (size_t k = 0; k < words.size(); ++k)
    {
        per[k] = issue(w, decode(words[k]), kind);
    }
    return per;
}

struct PairStats
{
    int stalls = 0;      // stalls charged to the addi of every la pair
    int adjacent = 0;    // pairs whose addi still directly follows its auipc
};

PairStats pairStats(const std::vector<uint32_t> &originalWords, const ScheduleReport *report, PipelineKind kind)
{
    const std::vector<uint32_t> &words = report ? report->words : originalWords;
    std::vector<size_t> position(words.size());
    for (size_t k = 0; k < position.size(); ++k)
    {
        position[k] = k;
    }
    if (report)
    {
        const std::vector<size_t> original = originalIndexOf(*report);
        for (size_t pos = 0; pos < original.size(); ++pos)
        {
            position[original[pos]] = pos;
        }
    }
    const std::vector<int> per = stallsPerWord(words, kind);
    PairStats s;
    for (size_t i : laPairs(originalWords))
    {
        s.stalls += per[position[i + 1]];
        s.adjacent += (position[i + 1] == position[i] + 1) ? 1 : 0;
    }
    return s;
}

struct SetTotals
{
    size_t programs = 0, blocks = 0, pairs = 0;
    long long predOriginal = 0, predGreedy = 0, predOptimal = 0;
    long long vmOriginal = 0, vmGreedy = 0, vmOptimal = 0;
    PairStats pairOriginal, pairGreedy, pairOptimal;
};

template <typename VmType>
void reportSet(const char *setName, const std::vector<NamedProgram> &programs, PipelineKind kind, bool runVm,
               bool straightLine)
{
    SetTotals t;
    for (const NamedProgram &p : programs)
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const std::vector<uint32_t> &words = program.text_buffer;
        const std::vector<size_t> leaders = labelLeaders(program);
        const ScheduleReport greedy = scheduleProgram(words, kind, leaders);
        const ScheduleReport optimal = scheduleProgram(words, kind, leaders, true, ScheduleMode::Optimal);

        ++t.programs;
        t.blocks += findBlocks(words, leaders).size();
        t.predOriginal += greedy.stallsBefore;
        t.predGreedy += greedy.stallsAfter;
        t.predOptimal += optimal.stallsAfter;

        if (runVm)
        {
            const Kites::RunResult a = Kites::runProgram<VmType>(program, {}, kCycleCap);
            const Kites::RunResult g = Kites::runProgram<VmType>(Kites::withText(program, greedy.words), {}, kCycleCap);
            const Kites::RunResult o = Kites::runProgram<VmType>(Kites::withText(program, optimal.words), {}, kCycleCap);
            EXPECT_TRUE(Kites::sameState(a, g)) << p.name << " greedy (" << pipelineName(kind) << ")";
            EXPECT_TRUE(Kites::sameState(a, o)) << p.name << " optimal (" << pipelineName(kind) << ")";
            EXPECT_EQ(a.retired, o.retired) << p.name;
            EXPECT_LE(o.stalls, a.stalls) << p.name;
            t.vmOriginal += a.stalls;
            t.vmGreedy += g.stalls;
            t.vmOptimal += o.stalls;
        }
        if (straightLine)
        {
            t.pairs += laPairs(words).size();
            const PairStats so = pairStats(words, nullptr, kind);
            const PairStats sg = pairStats(words, &greedy, kind);
            const PairStats sp = pairStats(words, &optimal, kind);
            t.pairOriginal.stalls += so.stalls;
            t.pairOriginal.adjacent += so.adjacent;
            t.pairGreedy.stalls += sg.stalls;
            t.pairGreedy.adjacent += sg.adjacent;
            t.pairOptimal.stalls += sp.stalls;
            t.pairOptimal.adjacent += sp.adjacent;
        }
    }

    std::cout << "[PIN-REPORT] " << setName << " (" << pipelineName(kind) << "): programs=" << t.programs
              << " blocks=" << t.blocks << " | predicted original " << t.predOriginal << ", greedy "
              << t.predGreedy << ", optimal " << t.predOptimal;
    if (runVm)
    {
        std::cout << " | VM original " << t.vmOriginal << ", greedy " << t.vmGreedy << ", optimal " << t.vmOptimal;
    }
    std::cout << std::endl;
    if (straightLine)
    {
        std::cout << "[PIN-REPORT]   la pairs=" << t.pairs << ": stalls on the paired addi original "
                  << t.pairOriginal.stalls << ", greedy " << t.pairGreedy.stalls << ", optimal "
                  << t.pairOptimal.stalls << " | still adjacent: original " << t.pairOriginal.adjacent
                  << ", greedy " << t.pairGreedy.adjacent << ", optimal " << t.pairOptimal.adjacent << std::endl;
    }
}

} // namespace

// Before/after report for pinning auipc: blocks, predicted and VM stalls,
// and the stalls charged to the addi of each la pair.
TEST(ReorderPinning, Report)
{
    const std::vector<NamedProgram> examples =
        programsIn({kRepoRoot / "examples", kRepoRoot / "examples" / "fd-extension"});
    const std::vector<NamedProgram> demo = programsIn({kRepoRoot / "test" / "reorder_demo"});
    const std::vector<NamedProgram> la = laPrograms(8080u, 100);

    reportSet<Kites::RV5StageProcessorHF>("examples", examples, PipelineKind::HF, false, false);
    reportSet<Kites::RV5StageProcessorHF>("demo", demo, PipelineKind::HF, true, false);
    reportSet<Kites::RV5StageProcessorHF>("random la", la, PipelineKind::HF, true, true);
    reportSet<Kites::RV5StageProcessorHNF>("examples", examples, PipelineKind::HNF, false, false);
    reportSet<Kites::RV5StageProcessorHNF>("demo", demo, PipelineKind::HNF, true, false);
    reportSet<Kites::RV5StageProcessorHNF>("random la", la, PipelineKind::HNF, true, true);
}

// =======================================================================
// Pinning tests.
// =======================================================================

namespace
{

// Straight-line programs with auipc mixed in: fixed data bases x10-x12 set
// first (never overwritten), auipc writing pool registers with random
// immediates (so its rs1/rs2 bits are random too and can hit the quirk),
// loads/stores off the fixed bases, ALU work. Readers and writers of auipc's
// rd give it ordinary predecessors (WAR/WAW) and successors (RAW/WAW).
std::string makePinnedProgram(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "mul"};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    std::ostringstream out;
    for (int i = 0; i < 3; ++i)
    {
        out << "addi x" << baseReg[i] << ", x0, " << baseAddr[i] << "\n";
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(9);
        const int rd = pool[pick(8)], rs1 = pool[pick(8)], rs2 = pool[pick(8)];
        if (op <= 1)
        {
            out << "auipc x" << rd << ", " << pick(1 << 20) << "\n";
        }
        else if (op == 2)
        {
            out << "lw x" << rd << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else if (op == 3)
        {
            out << "sw x" << rs2 << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else if (op == 4)
        {
            out << "addi x" << rd << ", x" << rs1 << ", " << ((pick(64) << 5) | pool[pick(8)]) << "\n";
        }
        else
        {
            out << rOps[pick(6)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

std::vector<NamedProgram> pinnedPrograms(unsigned seed, int count, int minLen, int maxLen)
{
    std::mt19937 rng(seed);
    std::vector<NamedProgram> programs;
    for (int t = 0; t < count; ++t)
    {
        const int len = std::uniform_int_distribution<int>(minLen, maxLen)(rng);
        programs.push_back({"pinned#" + std::to_string(t), makePinnedProgram(rng, len)});
    }
    return programs;
}

std::vector<DecodedInst> decodeBlock(const std::vector<uint32_t> &words, const Block &block)
{
    std::vector<DecodedInst> insts;
    for (size_t k = block.begin; k < block.end; ++k)
    {
        insts.push_back(decode(words[k]));
    }
    return insts;
}

size_t pinCount(const std::vector<DecodedInst> &insts)
{
    return static_cast<size_t>(
        std::count_if(insts.begin(), insts.end(), [](const DecodedInst &d) { return d.is_pinned; }));
}

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

// A legal order of the block: a permutation that respects every edge and
// leaves every pinned instruction at its own slot.
::testing::AssertionResult legalWithPins(const std::vector<size_t> &order, const DepGraph &g,
                                         const std::vector<DecodedInst> &insts)
{
    if (order.size() != g.size)
    {
        return ::testing::AssertionFailure() << "wrong size";
    }
    std::vector<size_t> position(order.size(), order.size());
    for (size_t pos = 0; pos < order.size(); ++pos)
    {
        if (order[pos] >= order.size() || position[order[pos]] != order.size())
        {
            return ::testing::AssertionFailure() << "not a permutation";
        }
        position[order[pos]] = pos;
    }
    for (size_t i = 0; i < g.size; ++i)
    {
        if (insts[i].is_pinned && position[i] != i)
        {
            return ::testing::AssertionFailure() << "pinned instruction " << i << " moved to " << position[i];
        }
        for (size_t j : g.succ[i])
        {
            if (position[i] >= position[j])
            {
                return ::testing::AssertionFailure() << "edge " << i << "->" << j << " violated";
            }
        }
    }
    return ::testing::AssertionSuccess();
}

// May `i` go into `slot` next (ready, and the pin rule)?
bool allowed(size_t i, size_t slot, const std::vector<bool> &placed, const DepGraph &g,
             const std::vector<DecodedInst> &insts)
{
    if (placed[i])
    {
        return false;
    }
    for (size_t p : g.pred[i])
    {
        if (!placed[p])
        {
            return false;
        }
    }
    return insts[slot].is_pinned ? i == slot : !insts[i].is_pinned;
}

// Brute force: can the unplaced instructions fill slots slot..n-1 legally?
bool bruteCanComplete(std::vector<bool> &placed, size_t slot, const DepGraph &g,
                      const std::vector<DecodedInst> &insts)
{
    if (slot == insts.size())
    {
        return true;
    }
    for (size_t i = 0; i < insts.size(); ++i)
    {
        if (allowed(i, slot, placed, g, insts))
        {
            placed[i] = true;
            const bool ok = bruteCanComplete(placed, slot + 1, g, insts);
            placed[i] = false;
            if (ok)
            {
                return true;
            }
        }
    }
    return false;
}

// Every legal order (pins at their slots), the minimum predicted stalls.
int naiveMinimumWithPins(const std::vector<DecodedInst> &insts, const DepGraph &g, PipelineKind kind,
                         const HeadContext &ctx)
{
    std::vector<bool> placed(insts.size(), false);
    std::vector<DecodedInst> order;
    int best = 1 << 30;
    std::function<void()> enumerate = [&]()
    {
        const size_t slot = order.size();
        if (slot == insts.size())
        {
            best = std::min(best, predictStalls(order, kind, ctx));
            return;
        }
        for (size_t i = 0; i < insts.size(); ++i)
        {
            if (allowed(i, slot, placed, g, insts))
            {
                placed[i] = true;
                order.push_back(insts[i]);
                enumerate();
                order.pop_back();
                placed[i] = false;
            }
        }
    };
    enumerate();
    return best;
}

std::vector<uint32_t> lastWords(const std::vector<uint32_t> &words, size_t k)
{
    return std::vector<uint32_t>(words.end() - static_cast<std::ptrdiff_t>(k), words.end());
}

} // namespace

// PinnedSlots::canComplete must say "possible" exactly when a brute-force
// search finds a legal way to finish, in every reachable partial order.
TEST(ReorderPinning, CanCompleteMatchesBruteForce)
{
    std::mt19937 rng(9001u);
    size_t blocks = 0, states = 0, infeasible = 0;
    while (blocks < 300)
    {
        const int k = 4 + static_cast<int>(blocks % 6);   // 4..9 instructions
        const Kites::AssembledProgram program = assembleSource(makePinnedProgram(rng, k));
        const std::vector<uint32_t> words = lastWords(program.text_buffer, static_cast<size_t>(k));
        const Block block{0, words.size(), false};
        const std::vector<DecodedInst> insts = decodeBlock(words, block);
        if (pinCount(insts) == 0)
        {
            continue;
        }
        ++blocks;
        const DepGraph g = buildDepGraph(words, block);
        const PinnedSlots pins(insts, g);

        // Walk every partial order reachable by legal placements (without the
        // feasibility check), visiting each placed-set once.
        std::vector<bool> placed(insts.size(), false);
        std::vector<bool> seen(size_t{1} << insts.size(), false);
        std::function<void(size_t, uint32_t)> walk = [&](size_t slot, uint32_t mask)
        {
            if (seen[mask])
            {
                return;
            }
            seen[mask] = true;
            ++states;
            const bool expected = bruteCanComplete(placed, slot, g, insts);
            infeasible += expected ? 0 : 1;
            ASSERT_EQ(pins.canComplete(placed, slot, g), expected) << "block " << blocks << " slot " << slot;
            for (size_t i = 0; i < insts.size(); ++i)
            {
                if (allowed(i, slot, placed, g, insts))
                {
                    placed[i] = true;
                    walk(slot + 1, mask | (1u << i));
                    placed[i] = false;
                }
            }
        };
        walk(0, 0);
    }
    std::cout << "[PINNING] canComplete matched brute force on " << states << " partial orders of " << blocks
              << " blocks (" << infeasible << " of them dead ends)" << std::endl;
    EXPECT_GT(infeasible, 0u);
}

// Every greedy and optimal order keeps each pinned instruction at its slot
// and respects the dependency graph, on random la programs, random auipc
// programs and a hand-built block with three pins.
TEST(ReorderPinning, PinnedInstructionsKeepTheirSlot)
{
    std::vector<NamedProgram> programs = laPrograms(4242u, 100);
    for (const NamedProgram &p : pinnedPrograms(4343u, 100, 10, 30))
    {
        programs.push_back(p);
    }
    // Pins at 1, 3 and 6; WAR edges (reads of x6/x8/x14 before the auipc
    // that writes them) give the pins predecessors too.
    programs.push_back({"three pins",
                        "add x16, x6, x8\nauipc x6, 1\nlw x7, 0(x10)\nauipc x8, 2\nadd x9, x7, x7\n"
                        "addi x13, x6, 4\nauipc x14, 3\nadd x15, x9, x13\naddi x16, x8, 1\nadd x17, x14, x14\n"});

    size_t pinnedBlocks = 0, pins = 0, movedAround = 0;
    for (const NamedProgram &p : programs)
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const std::vector<uint32_t> &words = program.text_buffer;
        for (const Block &block : findBlocks(words, labelLeaders(program)))
        {
            const std::vector<DecodedInst> insts = decodeBlock(words, block);
            if (pinCount(insts) == 0)
            {
                continue;
            }
            ++pinnedBlocks;
            pins += pinCount(insts);
            const DepGraph g = buildDepGraph(words, block);
            Context c;
            fillContext(c, words, block);
            for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
            {
                const std::vector<size_t> greedy = scheduleBlock(words, block, kind, c.ctx);
                const OptimalBlockResult optimal = scheduleBlockOptimalDetailed(words, block, kind, c.ctx);
                EXPECT_TRUE(legalWithPins(greedy, g, insts)) << p.name << " greedy " << pipelineName(kind);
                EXPECT_TRUE(legalWithPins(optimal.order, g, insts)) << p.name << " optimal " << pipelineName(kind);
                EXPECT_LE(optimal.optimalStalls, optimal.greedyStalls) << p.name;

                // Count blocks where something actually moved across a pin.
                for (size_t pos = 0; pos < greedy.size(); ++pos)
                {
                    if (insts[pos].is_pinned)
                    {
                        bool crossed = false;
                        for (size_t q = 0; q < pos; ++q) crossed = crossed || greedy[q] > pos;
                        movedAround += crossed ? 1 : 0;
                    }
                }
            }
        }
    }
    std::cout << "[PINNING] " << pinnedBlocks << " blocks with " << pins
              << " pinned instructions: all greedy and optimal orders keep them in place; an instruction "
              << "moved across a pin in " << movedAround << " (pin, pipeline) cases" << std::endl;
    EXPECT_GT(movedAround, 0u);
}

// On blocks of up to 7 instructions with at least one pinned instruction,
// the optimizer equals a full listing of the legal orders (pins in place).
TEST(ReorderPinning, OptimalMatchesNaiveWithPins)
{
    std::mt19937 rng(777u);
    size_t blocks = 0;
    while (blocks < 400)
    {
        const int k = 3 + static_cast<int>(blocks % 5);   // 3..7 instructions
        const Kites::AssembledProgram program = assembleSource(makePinnedProgram(rng, k + 2));
        const std::vector<uint32_t> &words = program.text_buffer;
        const Block block{words.size() - static_cast<size_t>(k), words.size(), false};
        const std::vector<DecodedInst> insts = decodeBlock(words, block);
        if (pinCount(insts) == 0)
        {
            continue;
        }
        ++blocks;
        const DepGraph g = buildDepGraph(words, block);
        Context c;
        fillContext(c, words, block);
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            for (const HeadContext &ctx : {c.ctx, HeadContext{}})
            {
                const OptimalBlockResult r = scheduleBlockOptimalDetailed(words, block, kind, ctx);
                EXPECT_TRUE(r.provenOptimal());
                EXPECT_EQ(r.optimalStalls, naiveMinimumWithPins(insts, g, kind, ctx)) << pipelineName(kind);
                EXPECT_TRUE(legalWithPins(r.order, g, insts));
            }
        }
    }
    std::cout << "[PINNING] optimal matched the full listing on " << blocks << " small blocks with pins"
              << std::endl;
}

// The cost model must still match the VM exactly on straight-line programs
// that contain auipc (no earlier randomized test used it).
TEST(ReorderPinning, CostModelMatchesVmWithAuipc)
{
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        long long total = 0;
        int auipcs = 0;
        for (const NamedProgram &p : pinnedPrograms(kind == PipelineKind::HF ? 111u : 222u, 200, 10, 25))
        {
            const Kites::AssembledProgram program = assembleSource(p.source);
            for (uint32_t w : program.text_buffer)
            {
                auipcs += decode(w).kind == Kind::Auipc ? 1 : 0;
            }
            const int predicted = predictStalls(program.text_buffer, kind);
            const Kites::RunResult r =
                kind == PipelineKind::HF ? Kites::runProgram<Kites::RV5StageProcessorHF>(program, {}, kCycleCap)
                                         : Kites::runProgram<Kites::RV5StageProcessorHNF>(program, {}, kCycleCap);
            ASSERT_EQ(predicted, static_cast<int>(r.stalls)) << pipelineName(kind) << "\n" << p.source;
            total += predicted;
        }
        std::cout << "[PINNING] cost model == VM on 200 programs with " << auipcs << " auipc ("
                  << pipelineName(kind) << "), " << total << " stalls" << std::endl;
    }
}

// VM equivalence of greedy and optimal schedules on random programs with
// `la` in the middle of blocks: same registers, data and retired count,
// never more stalls, and every auipc word still at its original address.
namespace
{

template <typename VmType>
void laEquivalence(PipelineKind kind)
{
    int pairs = 0, separated = 0;
    for (const NamedProgram &p : laPrograms(kind == PipelineKind::HF ? 5150u : 5151u, 100))
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const std::vector<uint32_t> &words = program.text_buffer;
        const std::vector<size_t> leaders = labelLeaders(program);
        const Kites::RunResult original = Kites::runProgram<VmType>(program, {}, kCycleCap);
        for (ScheduleMode mode : {ScheduleMode::Greedy, ScheduleMode::Optimal})
        {
            const ScheduleReport report = scheduleProgram(words, kind, leaders, true, mode);
            const Kites::RunResult after = Kites::runProgram<VmType>(Kites::withText(program, report.words), {}, kCycleCap);
            const std::string tag = p.name + (mode == ScheduleMode::Greedy ? " greedy " : " optimal ") + pipelineName(kind);
            EXPECT_TRUE(Kites::sameState(original, after)) << tag;
            EXPECT_EQ(original.retired, after.retired) << tag;
            EXPECT_LE(after.stalls, original.stalls) << tag;
            for (size_t i = 0; i < words.size(); ++i)
            {
                if (decode(words[i]).is_pinned)
                {
                    EXPECT_EQ(report.words[i], words[i]) << tag << ": auipc moved from " << i;
                }
            }
            if (mode == ScheduleMode::Optimal)
            {
                const std::vector<size_t> fromIndex = originalIndexOf(report);
                std::vector<size_t> position(fromIndex.size());
                for (size_t pos = 0; pos < fromIndex.size(); ++pos) position[fromIndex[pos]] = pos;
                for (size_t i : laPairs(words))
                {
                    ++pairs;
                    separated += position[i + 1] != i + 1 ? 1 : 0;
                }
            }
        }
    }
    std::cout << "[PINNING] la VM equivalence (" << pipelineName(kind) << "): 100 programs, " << pairs
              << " la pairs, addi moved away from its auipc in " << separated << " (optimal)" << std::endl;
}

} // namespace

TEST(ReorderPinning, VmEquivalenceLaPrograms_HF) { laEquivalence<Kites::RV5StageProcessorHF>(PipelineKind::HF); }

TEST(ReorderPinning, VmEquivalenceLaPrograms_HNF) { laEquivalence<Kites::RV5StageProcessorHNF>(PipelineKind::HNF); }
