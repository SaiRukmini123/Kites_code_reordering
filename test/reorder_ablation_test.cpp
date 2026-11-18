#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
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
#include "reorder/asm_writer.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder_la_programs.h"
#include "utils/utils.h"

// Quirk ablation: schedule with the exact hazard model (what the VM does:
// rs1/rs2 bit fields of every instruction, immediates included) and with a
// textbook model (only registers an instruction really reads), and measure
// both on the real VMs.

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
const char *modelName(HazardModel m) { return m == HazardModel::Exact ? "exact" : "textbook"; }

// ---------------------------------------------------------------------
// Program sets
// ---------------------------------------------------------------------
auto picker(std::mt19937 &rng)
{
    return [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
}

// Same op mix and safe data bases as the scheduler and optimizer tests:
// half the addi immediates have low bits that name a live register.
std::string makeStraightLine(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "sll", "srl", "sra", "mul", "div", "rem"};
    auto pick = picker(rng);
    std::ostringstream out;
    for (int i = 0; i < 3; ++i) out << "addi x" << baseReg[i] << ", x0, " << baseAddr[i] << "\n";
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(14);
        const int rd = pool[pick(13)], rs1 = pool[pick(13)], rs2 = pool[pick(13)];
        if (op == 0)
        {
            int imm = pick(2048);
            if (pick(2) == 1) imm = (imm & ~0x1F) | pool[pick(13)];
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else if (op == 12) out << "lw x" << rd << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        else if (op == 13) out << "sw x" << rs2 << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        else out << rOps[op - 1] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
    }
    return out.str();
}

// Straight-line code with forward-only branches and jumps.
std::string makeBranchy(std::mt19937 &rng, int bodyLen)
{
    static const char *branchOps[] = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};
    auto pick = picker(rng);
    std::vector<int> labelPos;
    for (int k = 0, count = 1 + pick(3); k < count; ++k) labelPos.push_back(1 + pick(bodyLen));
    std::sort(labelPos.begin(), labelPos.end());
    std::istringstream body(makeStraightLine(rng, bodyLen));
    std::vector<std::string> lines;
    for (std::string line; std::getline(body, line);) lines.push_back(line);
    std::ostringstream out;
    for (int i = 0; i < 3; ++i) out << lines[static_cast<size_t>(i)] << "\n";
    for (int i = 0; i < bodyLen; ++i)
    {
        for (size_t k = 0; k < labelPos.size(); ++k)
            if (labelPos[k] == i) out << "skip" << k << ":\n";
        std::vector<size_t> forward;
        for (size_t k = 0; k < labelPos.size(); ++k)
            if (labelPos[k] > i) forward.push_back(k);
        if (!forward.empty() && pick(6) == 0)
        {
            const size_t k = forward[static_cast<size_t>(pick(static_cast<int>(forward.size())))];
            if (pick(3) == 0) out << "jal x0, skip" << k << "\n";
            else out << branchOps[pick(6)] << " x" << (5 + pick(4)) << ", x" << (13 + pick(4)) << ", skip" << k << "\n";
        }
        else out << lines[static_cast<size_t>(3 + i)] << "\n";
    }
    for (size_t k = 0; k < labelPos.size(); ++k)
        if (labelPos[k] == bodyLen) out << "skip" << k << ":\n";
    return out.str();
}

// Immediate-heavy: the destination registers are x4, x8, ..., x28 and x5-x7,
// so small addi immediates (0-31) and word-aligned load offsets (0-28) put a
// live register number into the rs2 bit field all the time, right next to
// the loads that produce those registers.
std::string makeImmediateHeavy(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 13};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {4, 8, 12, 16, 20, 24, 28, 5, 6, 7};
    auto pick = picker(rng);
    std::ostringstream out;
    for (int i = 0; i < 3; ++i) out << "addi x" << baseReg[i] << ", x0, " << baseAddr[i] << "\n";
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(20);
        const int rd = pool[pick(10)], rs1 = pool[pick(10)], rs2 = pool[pick(10)];
        if (op < 7) out << "lw x" << rd << ", " << 4 * pick(8) << "(x" << baseReg[pick(3)] << ")\n";
        else if (op < 12) out << "addi x" << rd << ", x" << rs1 << ", " << pick(32) << "\n";
        else if (op < 15) out << "sw x" << rs2 << ", " << 4 * pick(8) << "(x" << baseReg[pick(3)] << ")\n";
        else out << (pick(2) ? "add" : "mul") << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
    }
    return out.str();
}

std::vector<NamedProgram> randomSet(const char *prefix, std::string (*make)(std::mt19937 &, int), unsigned seed,
                                    int count, int minLen, int maxLen)
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

struct ProgramSet
{
    const char *name;
    std::vector<NamedProgram> programs;
    bool straightLine;   // one block, no branches: static predictions are exact for the VM
};

std::vector<ProgramSet> programSets()
{
    return {
        {"examples", programsIn({kRepoRoot / "examples", kRepoRoot / "examples" / "fd-extension"}), false},
        {"demo", programsIn({kRepoRoot / "test" / "reorder_demo"}), false},
        {"random straight-line", randomSet("straight", makeStraightLine, 13579u, 100, 10, 20), true},
        {"random branchy", randomSet("branchy", makeBranchy, 777u, 100, 10, 25), false},
        {"random la", reorder_test::laPrograms(8080u, 100), true},
        {"random immediate-heavy", randomSet("imm", makeImmediateHeavy, 5555u, 100, 12, 24), true},
    };
}

// ---------------------------------------------------------------------
// The experiment
// ---------------------------------------------------------------------
enum Column
{
    kOriginal,
    kTextbookGreedy,
    kExactGreedy,
    kTextbookOptimal,
    kExactOptimal,
    kColumns
};
const char *kColumnNames[kColumns] = {"original", "textbook-greedy", "exact-greedy", "textbook-optimal",
                                      "exact-optimal"};

ScheduleReport schedule(const Kites::AssembledProgram &program, PipelineKind kind, ScheduleMode mode,
                        HazardModel model)
{
    return scheduleProgram(program.text_buffer, kind, labelLeaders(program), true, mode, ContextMode::BlockEntries,
                           kDefaultLoopWeight, model);
}

// Stalls each instruction of a straight-line word stream is charged by a model.
std::vector<int> stallsPerWord(const std::vector<uint32_t> &words, PipelineKind kind, HazardModel model)
{
    std::vector<int> per(words.size(), 0);
    WindowState w;
    for (size_t k = 0; k < words.size(); ++k) per[k] = issue(w, decode(words[k]), kind, model);
    return per;
}

struct Example
{
    std::string name;
    std::vector<uint32_t> words;
    size_t at = 0;
    int exactStalls = 0;
    unsigned textbookVm = 0, exactVm = 0;
};

struct SetResult
{
    size_t programs = 0, capped = 0;
    std::array<long long, kColumns> stalls{}, cycles{};
    int textbookGreedySlower = 0, textbookGreedyFaster = 0;
    int textbookOptimalSlower = 0, textbookOptimalFaster = 0;
    // straight-line sets only
    long long falseStallsGreedy = 0, falseStallsOptimal = 0;
    long long errorTextbook = 0, errorExact = 0;   // sum over every run of |VM stalls - predicted|
    long long errorRuns = 0;
    std::vector<Example> examples;
};

template <typename VM>
SetResult runSet(const ProgramSet &set, PipelineKind kind)
{
    SetResult r;
    for (const NamedProgram &p : set.programs)
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        const Kites::RunResult base = Kites::runProgram<VM>(program, {}, kCycleCap);
        if (base.hitCap)
        {
            ++r.capped;
            continue;
        }
        ++r.programs;

        std::array<std::vector<uint32_t>, kColumns> words;
        words[kOriginal] = program.text_buffer;
        words[kTextbookGreedy] = schedule(program, kind, ScheduleMode::Greedy, HazardModel::Textbook).words;
        words[kExactGreedy] = schedule(program, kind, ScheduleMode::Greedy, HazardModel::Exact).words;
        words[kTextbookOptimal] = schedule(program, kind, ScheduleMode::Optimal, HazardModel::Textbook).words;
        words[kExactOptimal] = schedule(program, kind, ScheduleMode::Optimal, HazardModel::Exact).words;

        std::array<Kites::RunResult, kColumns> runs;
        runs[kOriginal] = base;
        for (int c = 1; c < kColumns; ++c)
        {
            runs[c] = Kites::runProgram<VM>(Kites::withText(program, words[c]), {}, kCycleCap);
            // The dependency graph is the same for both models, so every
            // schedule must still compute the same thing.
            EXPECT_TRUE(Kites::sameState(base, runs[c])) << p.name << " " << kColumnNames[c] << " " << pipelineName(kind);
            EXPECT_EQ(base.retired, runs[c].retired) << p.name << " " << kColumnNames[c];
        }
        for (int c = 0; c < kColumns; ++c)
        {
            r.stalls[c] += runs[c].stalls;
            r.cycles[c] += runs[c].cycles;
        }
        r.textbookGreedySlower += runs[kTextbookGreedy].cycles > runs[kExactGreedy].cycles;
        r.textbookGreedyFaster += runs[kTextbookGreedy].cycles < runs[kExactGreedy].cycles;
        r.textbookOptimalSlower += runs[kTextbookOptimal].cycles > runs[kExactOptimal].cycles;
        r.textbookOptimalFaster += runs[kTextbookOptimal].cycles < runs[kExactOptimal].cycles;

        if (!set.straightLine)
        {
            continue;
        }
        // On straight-line code the exact optimum is the true VM optimum, so
        // no textbook schedule can beat it on the VM.
        EXPECT_LE(runs[kExactOptimal].stalls, runs[kTextbookOptimal].stalls) << p.name << " " << pipelineName(kind);

        for (int c = 0; c < kColumns; ++c)
        {
            const int textbook = predictStalls(words[c], kind, HazardModel::Textbook);
            const int exact = predictStalls(words[c], kind, HazardModel::Exact);
            r.errorTextbook += std::abs(static_cast<int>(runs[c].stalls) - textbook);
            r.errorExact += std::abs(static_cast<int>(runs[c].stalls) - exact);
            ++r.errorRuns;
            if (c == kTextbookGreedy) r.falseStallsGreedy += static_cast<int>(runs[c].stalls) - textbook;
            if (c == kTextbookOptimal) r.falseStallsOptimal += static_cast<int>(runs[c].stalls) - textbook;
        }

        // Remember textbook-optimal schedules that walk into a false stall the
        // exact schedule avoids, to show a couple of them.
        if (runs[kTextbookOptimal].stalls > runs[kExactOptimal].stalls)
        {
            const std::vector<int> exact = stallsPerWord(words[kTextbookOptimal], kind, HazardModel::Exact);
            const std::vector<int> textbook = stallsPerWord(words[kTextbookOptimal], kind, HazardModel::Textbook);
            for (size_t k = 0; k < exact.size(); ++k)
            {
                if (exact[k] > textbook[k])
                {
                    r.examples.push_back({p.name, words[kTextbookOptimal], k, exact[k], runs[kTextbookOptimal].stalls,
                                          runs[kExactOptimal].stalls});
                    break;
                }
            }
        }
    }
    return r;
}

void printExample(const Example &e, PipelineKind kind)
{
    std::cout << "[ABLATION]   example " << e.name << " (" << pipelineName(kind) << "): textbook-optimal schedule "
              << e.textbookVm << " VM stalls vs exact-optimal " << e.exactVm << ". Around the false stall:\n";
    const size_t from = e.at >= 3 ? e.at - 3 : 0;
    for (size_t k = from; k <= e.at; ++k)
    {
        const DecodedInst d = decode(e.words[k]);
        std::cout << "[ABLATION]     " << std::left << std::setw(24) << disassembleForKites(e.words[k]) << std::right;
        if (k == e.at)
        {
            std::cout << "<- " << e.exactStalls << " stall(s) on the VM, textbook predicted 0: raw rs1 = x"
                      << int(d.rs1) << (d.reads_rs1 ? " (read)" : " (immediate bits)") << ", raw rs2 = x" << int(d.rs2)
                      << (d.reads_rs2 ? " (read)" : " (immediate bits)");
        }
        std::cout << "\n";
    }
    std::cout << std::flush;
}

template <typename VM>
void experiment(PipelineKind kind)
{
    for (const ProgramSet &set : programSets())
    {
        SetResult r = runSet<VM>(set, kind);
        std::cout << "[ABLATION] " << set.name << " (" << pipelineName(kind) << "): " << r.programs << " programs"
                  << (r.capped ? " (+" + std::to_string(r.capped) + " that never finish, left out)" : std::string());
        for (int c = 0; c < kColumns; ++c)
        {
            std::cout << " | " << kColumnNames[c] << " " << r.stalls[c] << " stalls / " << r.cycles[c] << " cycles";
        }
        std::cout << " | textbook slower than exact: greedy " << r.textbookGreedySlower << " (faster "
                  << r.textbookGreedyFaster << "), optimal " << r.textbookOptimalSlower << " (faster "
                  << r.textbookOptimalFaster << ") programs" << std::endl;
        if (set.straightLine)
        {
            std::cout << "[ABLATION]   false stalls the textbook model didn't predict: greedy " << r.falseStallsGreedy
                      << ", optimal " << r.falseStallsOptimal << " | prediction error (sum of |VM - predicted| over "
                      << r.errorRuns << " runs): textbook " << r.errorTextbook << ", exact " << r.errorExact
                      << std::endl;
            EXPECT_EQ(r.errorExact, 0) << set.name;
        }
        std::sort(r.examples.begin(), r.examples.end(),
                  [](const Example &a, const Example &b) { return a.words.size() < b.words.size(); });
        for (size_t i = 0; i < r.examples.size() && i < 2; ++i)
        {
            printExample(r.examples[i], kind);
        }
    }
}

} // namespace

// Reports the experiment, and checks on every program: every textbook (and
// exact) schedule is VM-equivalent to the original; on straight-line sets,
// exact-optimal VM stalls <= textbook-optimal VM stalls and the exact model's
// prediction error is 0.
TEST(ReorderAblation, Experiment_HF) { experiment<Kites::RV5StageProcessorHF>(PipelineKind::HF); }

TEST(ReorderAblation, Experiment_HNF) { experiment<Kites::RV5StageProcessorHNF>(PipelineKind::HNF); }

// =======================================================================
// When no immediate's bits ever name a live destination register, the two
// models see exactly the same hazards: identical schedules and stalls.
// Programs: addi immediates and load offsets are multiples of 32 (so the
// rs2 bit field is x0), conditional branches only (jal's fields are
// immediate bits), everything else really reads its rs1/rs2.
// =======================================================================

namespace
{

std::string makeNoCollision(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "mul"};
    static const char *branchOps[] = {"beq", "bne", "blt", "bge"};
    auto pick = picker(rng);
    std::ostringstream out;
    out << "addi x10, x0, 1024\naddi x11, x0, 1216\naddi x12, x0, 1408\n";   // multiples of 32 too
    int labels = 0;
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(10);
        const int rd = pool[pick(13)], rs1 = pool[pick(13)], rs2 = pool[pick(13)];
        if (op <= 1) out << "addi x" << rd << ", x" << rs1 << ", " << 32 * pick(32) << "\n";
        else if (op == 2) out << "lw x" << rd << ", " << 32 * pick(4) << "(x" << baseReg[pick(3)] << ")\n";
        else if (op == 3) out << "sw x" << rs2 << ", " << 4 * pick(8) << "(x" << baseReg[pick(3)] << ")\n";
        else if (op == 4 && i + 2 < bodyLen)
        {
            out << branchOps[pick(4)] << " x" << rs1 << ", x" << rs2 << ", L" << labels << "\n";
            out << rOps[pick(6)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
            out << "L" << labels++ << ":\n";
        }
        else out << rOps[pick(6)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
    }
    return out.str();
}

} // namespace

TEST(ReorderAblation, NoImmediateCollision_ModelsAgree)
{
    int programs = 0, moved = 0;
    for (const NamedProgram &p : randomSet("nocollision", makeNoCollision, 4242u, 150, 10, 25))
    {
        const Kites::AssembledProgram program = assembleSource(p.source);
        for (uint32_t w : program.text_buffer)   // the premise: no non-read field names a register
        {
            const DecodedInst d = decode(w);
            ASSERT_TRUE(d.reads_rs1 || d.rs1 == 0) << p.name << " " << disassembleForKites(w);
            ASSERT_TRUE(d.reads_rs2 || d.rs2 == 0) << p.name << " " << disassembleForKites(w);
        }
        ++programs;
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            for (ScheduleMode mode : {ScheduleMode::Greedy, ScheduleMode::Optimal})
            {
                const ScheduleReport exact = schedule(program, kind, mode, HazardModel::Exact);
                const ScheduleReport textbook = schedule(program, kind, mode, HazardModel::Textbook);
                EXPECT_EQ(exact.words, textbook.words) << p.name << " " << pipelineName(kind);
                EXPECT_EQ(exact.stallsBefore, textbook.stallsBefore) << p.name;
                EXPECT_EQ(exact.stallsAfter, textbook.stallsAfter) << p.name;
                ASSERT_EQ(exact.blocks.size(), textbook.blocks.size());
                for (size_t b = 0; b < exact.blocks.size(); ++b)
                {
                    EXPECT_EQ(exact.blocks[b].nodesSearched, textbook.blocks[b].nodesSearched) << p.name;
                }
                moved += exact.moves.empty() ? 0 : 1;
            }
        }
    }
    std::cout << "[ABLATION] no-collision programs: " << programs << " programs, both models gave identical "
              << "schedules, scores and node counts (" << moved << " of the schedules moved something)" << std::endl;
    EXPECT_GT(moved, 0);
}
