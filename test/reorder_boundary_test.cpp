#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder/vm_safety_net.h"
#include "reorder_la_programs.h"
#include "utils/utils.h"

using namespace Kites::reorder;
namespace fs = std::filesystem;

namespace
{

const fs::path kRepoRoot = fs::path(__FILE__).parent_path().parent_path();
constexpr unsigned kCycleCap = 100000;

using NamedProgram = reorder_test::LaProgram;

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

const char *pipelineName(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

template <typename VM>
Kites::RunResult runSource(const std::string &source)
{
    return Kites::runProgram<VM>(assembleSource(source), {}, kCycleCap);
}

} // namespace

// =======================================================================
// The flush experiment. Each case runs twice: with the target using the
// earlier result, and with an independent instruction instead; the cycle
// difference is the hazard that crosses the boundary.
//   - After a taken branch, jal or jalr the target starts clean: no hazard
//     on its first or second instruction, even from a load right before the
//     branch or from jal's own link register.
//   - A not-taken branch keeps the context: the branch fills one slot, so a
//     producer just above it is 2 back (rv5-h-nf: 1 stall, rv5-h-f: 0).
// =======================================================================

namespace
{

struct FlushCase
{
    const char *name;
    std::string dependent;
    std::string independent;
    int hazardHF;
    int hazardHNF;
};

std::vector<FlushCase> flushCases()
{
    const std::string pre = "addi x10, x0, 64\naddi x7, x0, 3\n";
    return {
        {"straight line: lw x5 -> use (control)", pre + "lw x5, 0(x10)\nadd x6, x5, x5\n",
         pre + "lw x5, 0(x10)\nadd x6, x7, x7\n", 1, 2},
        {"lw x5 -> beq taken -> target uses x5",
         pre + "lw x5, 0(x10)\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x5, x5\n",
         pre + "lw x5, 0(x10)\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x7, x7\n", 0, 0},
        {"lw x5 -> beq taken -> target 2nd instruction uses x5",
         pre + "lw x5, 0(x10)\nbeq x0, x0, T\naddi x9, x0, 1\nT:\naddi x8, x0, 2\nadd x6, x5, x5\n",
         pre + "lw x5, 0(x10)\nbeq x0, x0, T\naddi x9, x0, 1\nT:\naddi x8, x0, 2\nadd x6, x7, x7\n", 0, 0},
        {"lw x5 -> jal x0 -> target uses x5",
         pre + "lw x5, 0(x10)\njal x0, T\naddi x9, x0, 1\nT:\nadd x6, x5, x5\n",
         pre + "lw x5, 0(x10)\njal x0, T\naddi x9, x0, 1\nT:\nadd x6, x7, x7\n", 0, 0},
        {"lw x5 -> jalr x0 -> target uses x5",
         "addi x10, x0, 64\naddi x1, x0, 20\nlw x5, 0(x10)\njalr x0, 0(x1)\naddi x9, x0, 1\nadd x6, x5, x5\n",
         "addi x10, x0, 64\naddi x1, x0, 20\nlw x5, 0(x10)\njalr x0, 0(x1)\naddi x9, x0, 1\nadd x6, x10, x10\n", 0,
         0},
        {"addi x5 -> beq taken -> target uses x5",
         pre + "addi x5, x0, 9\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x5, x5\n",
         pre + "addi x5, x0, 9\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x7, x7\n", 0, 0},
        {"lw x5 -> addi -> beq taken -> target uses x5",
         pre + "lw x5, 0(x10)\naddi x8, x0, 2\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x5, x5\n",
         pre + "lw x5, 0(x10)\naddi x8, x0, 2\nbeq x0, x0, T\naddi x9, x0, 1\nT:\nadd x6, x7, x7\n", 0, 0},
        {"jal x1 -> target uses x1 (link register)", pre + "jal x1, T\naddi x9, x0, 1\nT:\nadd x6, x1, x1\n",
         pre + "jal x1, T\naddi x9, x0, 1\nT:\nadd x6, x7, x7\n", 0, 0},
        {"lw x5 -> bne NOT taken -> next uses x5", pre + "lw x5, 0(x10)\nbne x0, x0, T\nadd x6, x5, x5\nT:\n",
         pre + "lw x5, 0(x10)\nbne x0, x0, T\nadd x6, x7, x7\nT:\n", 0, 1},
        {"addi x5 -> bne NOT taken -> next uses x5", pre + "addi x5, x0, 9\nbne x0, x0, T\nadd x6, x5, x5\nT:\n",
         pre + "addi x5, x0, 9\nbne x0, x0, T\nadd x6, x7, x7\nT:\n", 0, 1},
    };
}

template <typename VM>
void flushCheck(PipelineKind kind)
{
    for (const FlushCase &c : flushCases())
    {
        const Kites::RunResult d = runSource<VM>(c.dependent);
        const Kites::RunResult i = runSource<VM>(c.independent);
        const int hazard = static_cast<int>(d.cycles) - static_cast<int>(i.cycles);
        std::cout << "[FLUSH] " << pipelineName(kind) << " " << std::left << std::setw(52) << c.name << std::right
                  << " cycles " << std::setw(3) << d.cycles << " vs " << std::setw(3) << i.cycles
                  << " -> hazard " << hazard << std::endl;
        EXPECT_EQ(hazard, kind == PipelineKind::HF ? c.hazardHF : c.hazardHNF) << c.name;
        EXPECT_EQ(d.retired, i.retired) << c.name;
    }
}

} // namespace

TEST(ReorderBoundary, FlushExperiment_HF) { flushCheck<Kites::RV5StageProcessorHF>(PipelineKind::HF); }

TEST(ReorderBoundary, FlushExperiment_HNF) { flushCheck<Kites::RV5StageProcessorHNF>(PipelineKind::HNF); }

// =======================================================================
// Entry rules and loop weights on a hand-built program.
// =======================================================================

TEST(ReorderBoundary, EntryRules_HandBuilt)
{
    // 0  addi x5, x0, 3        B0: program start (clean)
    // 1  addi x6, x0, 0
    // 2  LOOP: add x6, x6, x5  B1: fall-through from B0 (loop entry, 1) + back edge from 4 (inside loop, W)
    // 3  addi x5, x5, -1
    // 4  bne x5, x0, LOOP
    // 5  jal x1, FUNC          B2: fall-through from B1 (loop exit, 1)
    // 6  addi x7, x6, 1        B3: return point of the call at 5 (clean); no fall-through (jal above)
    // 7  jal x0, END
    // 8  FUNC: addi x8, x0, 5  B4: target of the jal at 5 + label (clean); no fall-through
    // 9  jalr x0, 0(x1)
    // 10 END: addi x9, x0, 1   B5: target of the jal at 7 + label (clean); no fall-through (jalr above)
    const Kites::AssembledProgram program = assembleSource(
        "addi x5, x0, 3\naddi x6, x0, 0\nLOOP:\nadd x6, x6, x5\naddi x5, x5, -1\nbne x5, x0, LOOP\n"
        "jal x1, FUNC\naddi x7, x6, 1\njal x0, END\nFUNC:\naddi x8, x0, 5\njalr x0, 0(x1)\nEND:\naddi x9, x0, 1\n");
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = labelLeaders(program);
    const std::vector<Block> blocks = findBlocks(words, leaders);
    ASSERT_EQ(blocks.size(), 6u);

    const std::vector<LoopRange> loops = findLoops(words);
    ASSERT_EQ(loops.size(), 1u);
    EXPECT_EQ(loops[0].first, 2u);
    EXPECT_EQ(loops[0].last, 4u);
    EXPECT_EQ(edgeWeight(4, 2, loops, 10), 10);   // back edge
    EXPECT_EQ(edgeWeight(2, 3, loops, 10), 10);   // fall-through inside the loop
    EXPECT_EQ(edgeWeight(1, 2, loops, 10), 1);    // loop entry
    EXPECT_EQ(edgeWeight(4, 5, loops, 10), 1);    // loop exit

    struct Expected
    {
        bool fallThrough;
        int fallThroughWeight;
        bool clean;
        int cleanWeight;
    };
    const Expected expected[] = {
        {false, 0, true, 1}, {true, 1, true, 10}, {true, 1, false, 0},
        {false, 0, true, 1}, {false, 0, true, 1}, {false, 0, true, 1},
    };
    for (int w : {10, 1})
    {
        const std::vector<BlockEntry> entries = blockEntries(words, blocks, leaders, w);
        for (size_t b = 0; b < blocks.size(); ++b)
        {
            EXPECT_EQ(entries[b].fallThrough, expected[b].fallThrough) << "block " << b;
            EXPECT_EQ(entries[b].clean, expected[b].clean) << "block " << b;
            EXPECT_EQ(entries[b].fallThroughWeight, expected[b].fallThroughWeight) << "block " << b;
            EXPECT_EQ(entries[b].cleanWeight, (w == 1 && expected[b].cleanWeight > 0) ? 1 : expected[b].cleanWeight)
                << "block " << b << " W=" << w;
        }
    }
}

// With W = 1 every weight is 1, so the weighted score equals the plain sum:
// stalls under each head context, plus the tail under each.
TEST(ReorderBoundary, WeightOneIsThePlainSum)
{
    const Kites::AssembledProgram program = assembleSource(
        "addi x21, x0, 3\nlw x5, 0(x0)\nLOOP:\nadd x6, x5, x5\nlw x7, 4(x0)\nadd x8, x7, x6\n"
        "addi x21, x21, -1\nbne x21, x0, LOOP\nadd x9, x8, x8\n");
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = labelLeaders(program);
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const ScheduleReport r = scheduleProgram(words, kind, leaders, true, ScheduleMode::Greedy,
                                                 ContextMode::BlockEntries, 1);
        const std::vector<Block> blocks = findBlocks(words, leaders);
        for (size_t b = 0; b < blocks.size(); ++b)
        {
            const BlockStalls &s = r.blocks[b];
            EXPECT_TRUE(s.entry.fallThroughWeight <= 1 && s.entry.cleanWeight <= 1 && s.tailWeight <= 1);
        }
        // Loop block (index 1): fall-through context = the lw above, plus clean.
        const Block &loop = blocks[1];
        std::vector<DecodedInst> insts;
        for (size_t k = loop.begin; k < loop.end; ++k) insts.push_back(decode(words[k]));
        const DecodedInst above1 = decode(words[loop.begin - 1]);
        const DecodedInst above2 = decode(words[loop.begin - 2]);
        WindowState ft = windowFrom(HeadContext{&above1, &above2});
        WindowState clean{};
        int plain = 0;
        for (WindowState *w : {&ft, &clean})
        {
            for (const DecodedInst &d : insts) plain += issue(*w, d, kind);
            plain += issue(*w, decode(words[loop.end]), kind);   // tail: the one instruction after the loop
        }
        EXPECT_EQ(r.blocks[1].stallsBefore, plain) << pipelineName(kind);
    }
}

// =======================================================================
// Random programs with a counted loop and forward branches. Data lives in a
// .data buffer reached with la, so sameState also compares it.
// =======================================================================

namespace
{

void emitOp(std::mt19937 &rng, std::ostringstream &out)
{
    static const int base[] = {10, 11, 12};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "sll", "mul", "div", "rem"};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    const int op = pick(12);
    const int rd = pool[pick(12)], rs1 = pool[pick(12)], rs2 = pool[pick(12)];
    if (op <= 1)
    {
        int imm = pick(2048);
        if (pick(2) == 1)
        {
            imm = (imm & ~0x1F) | pool[pick(12)];
        }
        out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
    }
    else if (op <= 4)
    {
        out << "lw x" << rd << ", " << 4 * pick(5) << "(x" << base[pick(3)] << ")\n";
    }
    else if (op == 5)
    {
        out << "sw x" << rs2 << ", " << 4 * pick(5) << "(x" << base[pick(3)] << ")\n";
    }
    else
    {
        out << rOps[pick(9)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
    }
}

std::string makeLoopProgram(std::mt19937 &rng)
{
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    std::ostringstream out;
    out << ".data\nbuf: .word";
    for (int k = 0; k < 24; ++k) out << (k ? ", " : " ") << (k + 1);
    out << "\n.text\nla x10, buf\naddi x11, x10, 32\naddi x12, x10, 64\n";
    for (int i = 0, n = 2 + pick(4); i < n; ++i) emitOp(rng, out);
    out << "addi x21, x0, " << (2 + pick(3)) << "\nLOOP:\n";
    const int body = 3 + pick(6);
    const int skipAt = pick(3) == 0 ? pick(body) : -1;
    for (int i = 0; i < body; ++i)
    {
        if (i == skipAt)
        {
            out << "beq x" << (5 + pick(4)) << ", x" << (13 + pick(4)) << ", SKIP\n";
        }
        emitOp(rng, out);
        if (i == skipAt + 1 && skipAt >= 0)
        {
            out << "SKIP:\n";
        }
    }
    if (skipAt == body - 1)
    {
        out << "SKIP:\n";
    }
    out << "addi x21, x21, -1\nbne x21, x0, LOOP\n";
    for (int i = 0, n = 2 + pick(4); i < n; ++i)
    {
        if (pick(4) == 0)
        {
            out << "bne x" << (5 + pick(4)) << ", x0, END\n";
        }
        emitOp(rng, out);
    }
    out << "END:\n";
    return out.str();
}

std::vector<NamedProgram> loopPrograms(unsigned seed, int count)
{
    std::mt19937 rng(seed);
    std::vector<NamedProgram> programs;
    for (int t = 0; t < count; ++t)
    {
        programs.push_back({"loop#" + std::to_string(t), makeLoopProgram(rng)});
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

// ---------------------------------------------------------------------
// Report: VM cycles for original, greedy and optimal under array-order
// scoring (before), block-entry scoring with W = 1 and W = 10 (after), and
// the W = 10 schedules through the safety net; plus how many programs got
// slower than the original in each column. The safety net is also checked
// here: it never ends slower than the original, every accepted step is
// non-increasing, and its words really run in its reported cycles.
// ---------------------------------------------------------------------
enum Column
{
    kArrayGreedy,
    kArrayOptimal,
    kW1Greedy,
    kW1Optimal,
    kW10Greedy,
    kW10Optimal,
    kNetGreedy,
    kNetOptimal,
    kColumns
};

const char *kColumnNames[kColumns] = {"array greedy", "array optimal", "W=1 greedy", "W=1 optimal",
                                      "W=10 greedy",  "W=10 optimal",  "net greedy", "net optimal"};

struct SetReport
{
    size_t programs = 0, capped = 0, rejectedBlocks = 0, triedBlocks = 0;
    long long original = 0;
    std::array<long long, kColumns> cycles{};
    std::array<int, kColumns> slower{};
    std::vector<std::string> slowerNames[kColumns];
};

template <typename VM>
void reportSet(const char *setName, const std::vector<NamedProgram> &programs, PipelineKind kind)
{
    SetReport r;
    for (const NamedProgram &p : programs)
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const std::vector<uint32_t> &words = program.text_buffer;
        const std::vector<size_t> leaders = labelLeaders(program);
        const Kites::RunResult base = Kites::runProgram<VM>(program, {}, kCycleCap);
        if (base.hitCap)
        {
            ++r.capped;
            continue;
        }
        ++r.programs;
        r.original += base.cycles;

        ScheduleReport reports[6];
        reports[kArrayGreedy] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Greedy, ContextMode::ArrayOrder);
        reports[kArrayOptimal] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Optimal, ContextMode::ArrayOrder);
        reports[kW1Greedy] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Greedy, ContextMode::BlockEntries, 1);
        reports[kW1Optimal] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Optimal, ContextMode::BlockEntries, 1);
        reports[kW10Greedy] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Greedy);
        reports[kW10Optimal] = scheduleProgram(words, kind, leaders, true, ScheduleMode::Optimal);

        auto record = [&](int column, const Kites::RunResult &run)
        {
            EXPECT_TRUE(Kites::sameState(base, run)) << p.name << " " << kColumnNames[column] << " "
                                                     << pipelineName(kind);
            r.cycles[column] += run.cycles;
            if (run.cycles > base.cycles)
            {
                ++r.slower[column];
                r.slowerNames[column].push_back(p.name);
            }
        };
        for (int c = 0; c < 6; ++c)
        {
            record(c, Kites::runProgram<VM>(Kites::withText(program, reports[c].words), {}, kCycleCap));
        }
        for (int c : {kW10Greedy, kW10Optimal})
        {
            const SafetyNetResult net = applyWithVmCheck<VM>(program, reports[c], {}, kCycleCap);
            EXPECT_LE(net.finalCycles, net.originalCycles) << p.name;
            for (const SafetyNetTrial &t : net.trials)
            {
                if (t.accepted)
                {
                    EXPECT_LE(t.cyclesWith, t.cyclesBefore) << p.name;
                }
            }
            const Kites::RunResult check = Kites::runProgram<VM>(Kites::withText(program, net.words), {}, kCycleCap);
            EXPECT_EQ(check.cycles, net.finalCycles) << p.name;
            r.triedBlocks += net.trials.size();
            r.rejectedBlocks += net.rejectedBlocks().size();
            record(c == kW10Greedy ? kNetGreedy : kNetOptimal, check);
        }
    }

    std::cout << "[BOUNDARY] " << setName << " (" << pipelineName(kind) << "): " << r.programs << " programs"
              << (r.capped ? " (+" + std::to_string(r.capped) + " that never finish, left out)" : std::string())
              << " | VM cycles original " << r.original;
    for (int c = 0; c < kColumns; ++c)
    {
        std::cout << " | " << kColumnNames[c] << " " << r.cycles[c] << " (slower: " << r.slower[c] << ")";
    }
    std::cout << " | safety net rejected " << r.rejectedBlocks << " of " << r.triedBlocks << " changed blocks"
              << std::endl;
    for (int c = 0; c < kColumns; ++c)
    {
        if (!r.slowerNames[c].empty())
        {
            std::cout << "[BOUNDARY]   slower with " << kColumnNames[c] << ":";
            for (const std::string &n : r.slowerNames[c]) std::cout << " " << n;
            std::cout << std::endl;
        }
    }
    EXPECT_EQ(r.slower[kNetGreedy], 0);
    EXPECT_EQ(r.slower[kNetOptimal], 0);
}

std::vector<std::pair<const char *, std::vector<NamedProgram>>> reportSets()
{
    return {
        {"examples", programsIn({kRepoRoot / "examples", kRepoRoot / "examples" / "fd-extension"})},
        {"demo", programsIn({kRepoRoot / "test" / "reorder_demo"})},
        {"random loop+branch", loopPrograms(31415u, 100)},
        {"random la", reorder_test::laPrograms(8080u, 100)},
    };
}

} // namespace

TEST(ReorderBoundary, Report_HF)
{
    for (const auto &[name, programs] : reportSets())
    {
        reportSet<Kites::RV5StageProcessorHF>(name, programs, PipelineKind::HF);
    }
}

TEST(ReorderBoundary, Report_HNF)
{
    for (const auto &[name, programs] : reportSets())
    {
        reportSet<Kites::RV5StageProcessorHNF>(name, programs, PipelineKind::HNF);
    }
}

// =======================================================================
// Context rules and the tail effect on hand-built programs (rv5-h-nf).
// =======================================================================

namespace
{

std::vector<uint32_t> blockWords(const std::vector<uint32_t> &words, const Block &b)
{
    return std::vector<uint32_t>(words.begin() + static_cast<std::ptrdiff_t>(b.begin),
                                 words.begin() + static_cast<std::ptrdiff_t>(b.end));
}

using HNF = Kites::RV5StageProcessorHNF;

} // namespace

// A jump target is entered clean. In array order its context is the
// never-executed `jal x5` above it, which writes x5, so the old scoring
// predicts a hazard on `add x6, x5, x5` and moves an instruction for nothing.
// Block-entry scoring knows the target starts clean and keeps it.
TEST(ReorderBoundary, ContextRules_JumpTargetStartsClean)
{
    const std::string source = "addi x10, x0, 64\nlw x5, 0(x10)\njal x0, T\njal x5, T\n"
                               "T:\nadd x6, x5, x5\naddi x7, x0, 1\n";
    const Kites::AssembledProgram program = assembleSource(source);
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = labelLeaders(program);
    const std::vector<Block> blocks = findBlocks(words, leaders);
    ASSERT_EQ(blocks.size(), 3u);
    const BlockEntry target = blockEntries(words, blocks, leaders)[2];
    EXPECT_FALSE(target.fallThrough);
    EXPECT_TRUE(target.clean);

    const ScheduleReport array = scheduleProgram(words, PipelineKind::HNF, leaders, true, ScheduleMode::Greedy,
                                                 ContextMode::ArrayOrder);
    const ScheduleReport entries = scheduleProgram(words, PipelineKind::HNF, leaders);
    EXPECT_NE(blockWords(array.words, blocks[2]), blockWords(words, blocks[2]));    // old: moved
    EXPECT_EQ(blockWords(entries.words, blocks[2]), blockWords(words, blocks[2]));  // new: kept

    const Kites::RunResult a = Kites::runProgram<HNF>(Kites::withText(program, array.words), {}, kCycleCap);
    const Kites::RunResult e = Kites::runProgram<HNF>(Kites::withText(program, entries.words), {}, kCycleCap);
    std::cout << "[BOUNDARY] jump target: VM cycles with array-order move " << a.cycles << ", kept " << e.cycles
              << std::endl;
    EXPECT_EQ(a.cycles, e.cycles);   // the old move bought nothing
}

// A loop header entered by fall-through once (just after `addi x5`, which it
// reads) and by the back edge on every later iteration (clean, weight 10).
// Under the fall-through context alone every good order costs the same, so
// array-order scoring keeps greedy's choice, which stalls on every back-edge
// entry. Block-entry scoring weighs the back edge 10x and puts the producer
// of x6 first, so later iterations run without stalls.
TEST(ReorderBoundary, ContextRules_LoopHeaderWeighsTheBackEdge)
{
    const std::string source = "addi x21, x0, 5\naddi x5, x0, 7\nLOOP:\nadd x6, x5, x5\nadd x9, x6, x6\n"
                               "addi x7, x0, 1\naddi x21, x21, -1\nbne x21, x0, LOOP\n";
    const Kites::AssembledProgram program = assembleSource(source);
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = labelLeaders(program);
    const std::vector<Block> blocks = findBlocks(words, leaders);
    ASSERT_EQ(blocks.size(), 2u);
    const BlockEntry header = blockEntries(words, blocks, leaders)[1];
    EXPECT_TRUE(header.fallThrough);
    EXPECT_EQ(header.fallThroughWeight, 1);
    EXPECT_TRUE(header.clean);
    EXPECT_EQ(header.cleanWeight, 10);

    const ScheduleReport array = scheduleProgram(words, PipelineKind::HNF, leaders, true, ScheduleMode::Optimal,
                                                 ContextMode::ArrayOrder);
    const ScheduleReport entries = scheduleProgram(words, PipelineKind::HNF, leaders, true, ScheduleMode::Optimal);
    EXPECT_NE(blockWords(array.words, blocks[1]), blockWords(entries.words, blocks[1]));

    const Kites::RunResult o = Kites::runProgram<HNF>(program, {}, kCycleCap);
    const Kites::RunResult a = Kites::runProgram<HNF>(Kites::withText(program, array.words), {}, kCycleCap);
    const Kites::RunResult e = Kites::runProgram<HNF>(Kites::withText(program, entries.words), {}, kCycleCap);
    std::cout << "[BOUNDARY] loop header (5 iterations): VM cycles original " << o.cycles << ", array-order optimal "
              << a.cycles << ", block-entry optimal " << e.cycles << std::endl;
    EXPECT_TRUE(Kites::sameState(o, e));
    EXPECT_LT(e.cycles, a.cycles);
}

// Tail effect: block 0 can fall through into a block whose first instruction
// reads x5 and can't move (a branch follows it). Without the tail every order
// of block 0 is stall-free inside the block, and greedy happens to leave
// `addi x5` 2 slots before the boundary (1 stall there on rv5-h-nf). With the
// tail the search moves it earlier.
TEST(ReorderBoundary, TailEffect)
{
    const std::string source = "addi x9, x0, 3\naddi x5, x0, 1\nlw x6, 64(x0)\nadd x7, x6, x6\n"
                               "NEXT:\nadd x8, x5, x5\nbeq x0, x0, END\nEND:\n";
    const Kites::AssembledProgram program = assembleSource(source);
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = labelLeaders(program);
    const std::vector<Block> blocks = findBlocks(words, leaders);
    ASSERT_GE(blocks.size(), 2u);

    const ScheduleReport array = scheduleProgram(words, PipelineKind::HNF, leaders, true, ScheduleMode::Optimal,
                                                 ContextMode::ArrayOrder);
    const ScheduleReport entries = scheduleProgram(words, PipelineKind::HNF, leaders, true, ScheduleMode::Optimal);
    EXPECT_EQ(array.blocks[0].tailWeight, 0);
    EXPECT_EQ(entries.blocks[0].tailWeight, 1);
    EXPECT_NE(blockWords(array.words, blocks[0]), blockWords(entries.words, blocks[0]));

    const Kites::RunResult a = Kites::runProgram<HNF>(Kites::withText(program, array.words), {}, kCycleCap);
    const Kites::RunResult e = Kites::runProgram<HNF>(Kites::withText(program, entries.words), {}, kCycleCap);
    std::cout << "[BOUNDARY] tail effect: VM cycles without tail " << a.cycles << ", with tail " << e.cycles
              << std::endl;
    EXPECT_LT(e.cycles, a.cycles);
}

// The optimizer still finds the true minimum of the weighted score (two
// weighted heads plus a weighted tail), checked against every legal order of
// small random blocks.
TEST(ReorderBoundary, OptimalMatchesNaiveWeightedScore)
{
    std::mt19937 rng(2718u);
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    int blocks = 0;
    for (int t = 0; t < 300; ++t)
    {
        std::ostringstream out;
        const int k = 2 + t % 5;   // 2..6 instructions in the block
        for (int i = 0; i < k + 4; ++i) emitOp(rng, out);
        const Kites::AssembledProgram program = assembleSource(out.str());
        const std::vector<uint32_t> &w = program.text_buffer;
        const Block block{2, 2 + static_cast<size_t>(k), false};
        const DecodedInst c1 = decode(w[1]), c0 = decode(w[0]);

        BlockScoring scoring;
        scoring.heads.push_back(ScoredHead{windowFrom(HeadContext{&c1, &c0}), pick(2) ? 10 : 1});
        scoring.heads.push_back(ScoredHead{WindowState{}, pick(2) ? 10 : 1});
        scoring.tail = {decode(w[block.end]), decode(w[block.end + 1])};
        scoring.tailWeight = pick(2) ? 10 : 1;

        std::vector<DecodedInst> insts;
        for (size_t i = block.begin; i < block.end; ++i) insts.push_back(decode(w[i]));
        const DepGraph g = buildDepGraph(w, block);
        std::vector<size_t> order(insts.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        int naive = 1 << 30;
        do
        {
            bool legal = true;
            std::vector<size_t> pos(order.size());
            for (size_t p = 0; p < order.size(); ++p) pos[order[p]] = p;
            for (size_t i = 0; i < g.size && legal; ++i)
                for (size_t j : g.succ[i]) legal = legal && pos[i] < pos[j];
            if (legal)
            {
                std::vector<DecodedInst> seq;
                for (size_t i : order) seq.push_back(insts[i]);
                naive = std::min(naive, scoreOrder(seq, PipelineKind::HNF, scoring));
            }
        } while (std::next_permutation(order.begin(), order.end()));

        const OptimalBlockResult r = scheduleBlockOptimalDetailed(w, block, PipelineKind::HNF, scoring);
        ASSERT_TRUE(r.provenOptimal());
        ASSERT_EQ(r.optimalStalls, naive) << out.str();
        ++blocks;
    }
    std::cout << "[BOUNDARY] optimal matched the full listing of the weighted score on " << blocks << " blocks"
              << std::endl;
}

// =======================================================================
// Safety net rejections. No natural program came out slower in the report,
// so the reports here are built by hand: block 0 gets a legal order that
// adds a load-use stall (slower), block 1 a legal order that removes one
// (faster). A third case swaps two dependent instructions (illegal), which
// the net must refuse because the final state changes; the loaded values are
// nonzero .data words, so the wrong order really computes something else.
// =======================================================================

TEST(ReorderBoundary, SafetyNet_RejectsHandBuiltSlowerAndWrongBlocks)
{
    // 0 auipc, 1 addi (la) | 2 lw x5, 3 addi x7, 4 add x6 || 5 lw x8, 6 add x9, 7 addi x11
    const std::string source = ".data\nvals: .word 7, 9\n.text\nla x10, vals\nlw x5, 0(x10)\naddi x7, x0, 1\n"
                               "add x6, x5, x5\nNEXT:\nlw x8, 4(x10)\nadd x9, x8, x8\naddi x11, x0, 2\n";
    const Kites::AssembledProgram program = assembleSource(source);
    const std::vector<uint32_t> &w = program.text_buffer;
    const std::vector<Block> blocks = findBlocks(w, labelLeaders(program));
    ASSERT_EQ(blocks.size(), 2u);

    auto handReport = [&](const std::vector<uint32_t> &words)
    {
        ScheduleReport r;
        r.words = words;
        for (const Block &b : blocks)
        {
            BlockStalls s;
            s.block = b;
            r.blocks.push_back(s);
        }
        return r;
    };

    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        // Block 0: addi x7 moved after the load's use (slower); block 1: addi x11
        // moved into the load-use slot (faster).
        const ScheduleReport mixed = handReport({w[0], w[1], w[2], w[4], w[3], w[5], w[7], w[6]});
        const SafetyNetResult net = kind == PipelineKind::HF
                                        ? applyWithVmCheck<Kites::RV5StageProcessorHF>(program, mixed)
                                        : applyWithVmCheck<HNF>(program, mixed);
        ASSERT_EQ(net.trials.size(), 2u);
        EXPECT_FALSE(net.trials[0].accepted);
        EXPECT_EQ(net.trials[0].reason, SafetyNetTrial::Reason::Slower);
        EXPECT_GT(net.trials[0].cyclesWith, net.trials[0].cyclesBefore);
        EXPECT_TRUE(net.trials[1].accepted);
        EXPECT_EQ(net.rejectedBlocks(), std::vector<size_t>{0});
        EXPECT_LT(net.finalCycles, net.originalCycles);
        EXPECT_EQ(std::vector<uint32_t>(net.words.begin(), net.words.begin() + 5),
                  std::vector<uint32_t>(w.begin(), w.begin() + 5));   // block 0 left as written
        std::cout << "[BOUNDARY] safety net (" << pipelineName(kind) << "): original " << net.originalCycles
                  << " cycles; block 0 would make it " << net.trials[0].cyclesWith << " (rejected: "
                  << safetyNetReasonName(net.trials[0].reason) << "), block 1 makes it " << net.trials[1].cyclesWith
                  << " (accepted); final " << net.finalCycles << std::endl;

        // Illegal: the use of x5 moved above the load.
        const ScheduleReport wrong = handReport({w[0], w[1], w[4], w[2], w[3], w[5], w[6], w[7]});
        const SafetyNetResult refused = kind == PipelineKind::HF
                                            ? applyWithVmCheck<Kites::RV5StageProcessorHF>(program, wrong)
                                            : applyWithVmCheck<HNF>(program, wrong);
        ASSERT_EQ(refused.trials.size(), 1u);
        EXPECT_EQ(refused.trials[0].reason, SafetyNetTrial::Reason::DifferentState);
        EXPECT_EQ(refused.words, w);
    }
}
