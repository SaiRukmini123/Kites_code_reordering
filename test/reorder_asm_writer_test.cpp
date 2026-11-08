#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/asm_writer.h"
#include "utils/utils.h"

using namespace Kites::reorder;
namespace fs = std::filesystem;

namespace
{

const fs::path kRepoRoot = fs::path(__FILE__).parent_path().parent_path();

struct NamedProgram
{
    std::string name;
    std::string source;
};

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Kites::AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

const char *pipelineName(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

// ---------------------------------------------------------------------
// Final architectural state. Text differs between original and scheduled
// programs, so memory is compared over windows that exclude it: everything
// from the end of the text up to 0x10000 (covers every address the
// examples, loops and random programs use), the data section, and the 1 KiB
// just below the initial stack pointer.
// ---------------------------------------------------------------------
struct FinalState
{
    std::vector<uint64_t> gprs;
    std::vector<uint64_t> fprs;
    std::vector<uint32_t> memWords;
    unsigned instructionsRetired = 0;
    bool terminated = false;
};

// Exposes the protected drain check so the test can run the same loop as
// RV5StageVM_Base::DebugRun(), but with a retirement cap.
template <typename VmType>
struct SteppableVm : VmType
{
    using VmType::is_pipeline_drained;
};

// Some examples end in a deliberate spin loop (`end: beq x0, x0, end`) and
// never finish, so DebugRun() would run forever. Those are stopped after
// kRetireCap retired instructions, by which point every example is sitting in
// its final spin loop, where registers and memory no longer change.
constexpr unsigned kRetireCap = 100000;

template <typename VmType>
FinalState runToEnd(const Kites::AssembledProgram &program)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<SteppableVm<VmType>>();
    vm->LoadProgram(program);
    const uint64_t initialSp = vm->registers_.ReadGpr(2);

    FinalState s;
    while (vm->instructions_retired_ < kRetireCap)
    {
        if (vm->program_counter_ >= vm->program_size_ && vm->is_pipeline_drained())
        {
            s.terminated = true;
            break;
        }
        vm->Step();
    }

    for (int r = 1; r <= 31; ++r)
    {
        s.gprs.push_back(vm->registers_.ReadGpr(r));
    }
    for (int r = 0; r <= 31; ++r)
    {
        s.fprs.push_back(vm->registers_.ReadFpr(r));
    }
    s.instructionsRetired = vm->instructions_retired_;

    const uint64_t textEnd = (program.text_buffer.size() * 4 + 3) & ~uint64_t{3};
    for (uint64_t a = textEnd; a < 0x10000; a += 4)
    {
        s.memWords.push_back(vm->memory_controller_.readWord(a));
    }
    const uint64_t dataStart = 0x10000000;
    for (uint64_t a = dataStart; a < dataStart + program.data_buffer.size() + 64; a += 4)
    {
        s.memWords.push_back(vm->memory_controller_.readWord(a));
    }
    if (initialSp >= 1024)
    {
        for (uint64_t a = (initialSp - 1024) & ~uint64_t{3}; a < initialSp; a += 4)
        {
            s.memWords.push_back(vm->memory_controller_.readWord(a));
        }
    }
    return s;
}

template <typename VmType>
void expectSameFinalState(const std::string &tag, const Kites::AssembledProgram &original,
                          const Kites::AssembledProgram &rewritten)
{
    FinalState a = runToEnd<VmType>(original);
    FinalState b = runToEnd<VmType>(rewritten);
    EXPECT_EQ(a.terminated, b.terminated) << tag << ": only one run finished";
    EXPECT_EQ(a.gprs, b.gprs) << tag << ": GPRs differ";
    EXPECT_EQ(a.fprs, b.fprs) << tag << ": FPRs differ";
    EXPECT_EQ(a.memWords, b.memWords) << tag << ": memory differs";
    EXPECT_EQ(a.instructionsRetired, b.instructionsRetired) << tag << ": instructions_retired_ differs";
}

// The real proof: schedule for each pipeline, write the .s, assemble it
// again, and require (1) exactly the scheduled words and the original data,
// (2) the same final state as the original on both VMs.
void roundTrip(const NamedProgram &p)
{
    SCOPED_TRACE(p.name);
    Kites::AssembledProgram program = assembleSource(p.source);

    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        ScheduleReport report = scheduleProgram(program.text_buffer, kind, labelLeaders(program));
        const std::string written = writeAssembly(p.source, program, report);
        const std::string tag = p.name + " (scheduled for " + pipelineName(kind) + ")";
        SCOPED_TRACE(written);

        Kites::AssembledProgram rewritten;
        try
        {
            rewritten = assembleSource(written);
        }
        catch (const std::exception &e)
        {
            ADD_FAILURE() << tag << ": written .s does not assemble: " << e.what();
            continue;
        }

        ASSERT_EQ(rewritten.text_buffer, report.words) << tag << ": re-assembled words differ";
        EXPECT_EQ(rewritten.data_buffer, program.data_buffer) << tag << ": data differs";

        expectSameFinalState<Kites::RV5StageProcessorHF>(tag + " on h_f", program, rewritten);
        expectSameFinalState<Kites::RV5StageProcessorHNF>(tag + " on h_nf", program, rewritten);
    }
}

// ---------------------------------------------------------------------
// Program sets.
// ---------------------------------------------------------------------
std::vector<NamedProgram> examplePrograms()
{
    std::vector<NamedProgram> programs;
    for (const fs::path &dir : {kRepoRoot / "examples", kRepoRoot / "examples" / "fd-extension"})
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

// examples/*_hazard.s (top level only).
std::vector<NamedProgram> hazardExamples()
{
    std::vector<NamedProgram> programs;
    for (const auto &entry : fs::directory_iterator(kRepoRoot / "examples"))
    {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > 9 &&
            name.compare(name.size() - 9, 9, "_hazard.s") == 0)
        {
            programs.push_back({name, readFile(entry.path())});
        }
    }
    std::sort(programs.begin(), programs.end(),
              [](const NamedProgram &a, const NamedProgram &b) { return a.name < b.name; });
    return programs;
}

// Same five loop programs as reorder_scheduler_test.cpp / reorder_mem_deps_test.cpp.
std::vector<NamedProgram> loopPrograms()
{
    return {
        {"loop_array_sum",
         "addi x20, x0, 1000\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "sw x21, 0(x20)\nsw x21, 4(x20)\nsw x21, 8(x20)\nsw x21, 12(x20)\n"
         "LOOP:\nbeq x23, x21, END\nlw x24, 0(x20)\nadd x22, x22, x24\n"
         "addi x20, x20, 4\naddi x23, x23, 1\njal x0, LOOP\nEND:\n"},
        {"loop_load_use_body",
         "addi x20, x0, 1100\naddi x21, x0, 3\naddi x22, x0, 0\n"
         "sw x21, 0(x20)\nsw x21, 4(x20)\nsw x21, 8(x20)\n"
         "LOOP2:\nbeq x22, x21, END2\nlw x24, 0(x20)\nadd x25, x24, x24\n"
         "addi x20, x20, 4\naddi x22, x22, 1\njal x0, LOOP2\nEND2:\n"},
        {"loop_store_then_load",
         "addi x20, x0, 1200\naddi x21, x0, 3\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "LOOP3:\nbeq x22, x21, END3\nsw x22, 0(x20)\nlw x24, 0(x20)\nadd x23, x23, x24\n"
         "addi x20, x20, 4\naddi x22, x22, 1\njal x0, LOOP3\nEND3:\n"},
        {"loop_inplace_inc_read_next",
         "addi x20, x0, 1500\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "addi x5, x0, 10\nsw x5, 0(x20)\naddi x5, x0, 20\nsw x5, 4(x20)\n"
         "addi x5, x0, 30\nsw x5, 8(x20)\naddi x5, x0, 40\nsw x5, 12(x20)\n"
         "addi x5, x0, 50\nsw x5, 16(x20)\n"
         "LOOPA:\nbeq x22, x21, ENDA\nlw x24, 0(x20)\naddi x24, x24, 1\nsw x24, 0(x20)\n"
         "lw x25, 4(x20)\nadd x23, x23, x25\naddi x20, x20, 4\naddi x22, x22, 1\n"
         "jal x0, LOOPA\nENDA:\n"},
        {"loop_copy_load_next",
         "addi x20, x0, 1700\naddi x21, x0, 4\naddi x22, x0, 0\naddi x23, x0, 0\n"
         "addi x5, x0, 7\nsw x5, 0(x20)\naddi x5, x0, 8\nsw x5, 4(x20)\n"
         "addi x5, x0, 9\nsw x5, 8(x20)\naddi x5, x0, 10\nsw x5, 12(x20)\n"
         "addi x5, x0, 11\nsw x5, 16(x20)\n"
         "LOOPB:\nbeq x22, x21, ENDB\nlw x24, 0(x20)\nsw x24, 32(x20)\nlw x25, 4(x20)\n"
         "add x23, x23, x25\naddi x20, x20, 4\naddi x22, x22, 1\njal x0, LOOPB\nENDB:\n"},
    };
}

// Random straight-line code (same op mix and safe data bases as the
// scheduler test) with forward-only branches and jals to labels placed
// later, so every program terminates.
std::string makeRandomBranchyProgram(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "sll", "srl", "sra", "mul", "div", "rem"};
    static const char *branchOps[] = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};

    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    std::vector<int> labelPos;
    const int labelCount = 1 + pick(3);
    for (int k = 0; k < labelCount; ++k)
    {
        labelPos.push_back(1 + pick(bodyLen));   // 1..bodyLen; bodyLen = end of program
    }
    std::sort(labelPos.begin(), labelPos.end());

    std::ostringstream out;
    for (int i = 0; i < 3; ++i)
    {
        out << "addi x" << baseReg[i] << ", x0, " << baseAddr[i] << "\n";
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

        const int op = pick(18);
        const int rd = pool[pick(12)], rs1 = pool[pick(12)], rs2 = pool[pick(12)];
        if (op >= 15 && !forward.empty())
        {
            const size_t k = forward[static_cast<size_t>(pick(static_cast<int>(forward.size())))];
            if (op == 17)
            {
                out << "jal x" << (pick(2) == 0 ? 0 : 1) << ", skip" << k << "\n";
            }
            else
            {
                out << branchOps[pick(6)] << " x" << rs1 << ", x" << rs2 << ", skip" << k << "\n";
            }
        }
        else if (op == 0 || op >= 15)
        {
            out << "addi x" << rd << ", x" << rs1 << ", " << (pick(4096) - 2048) << "\n";
        }
        else if (op == 12)
        {
            out << "lw x" << rd << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else if (op == 13)
        {
            out << "sw x" << rs2 << ", " << 4 * pick(5) << "(x" << baseReg[pick(3)] << ")\n";
        }
        else if (op == 14)
        {
            out << "slli x" << rd << ", x" << rs1 << ", " << pick(64) << "\n";
        }
        else
        {
            out << rOps[op - 1] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
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

} // namespace

// =======================================================================
// Writer details.
// =======================================================================

TEST(ReorderAsmWriter, BranchTargetsUseSourceLabels)
{
    const std::string source =
        "main:\naddi x1, x0, 1\nbeq x1, x0, DONE\naddi x2, x0, 2\nDONE:\naddi x3, x0, 3\n";
    Kites::AssembledProgram program = assembleSource(source);
    ScheduleReport report = scheduleProgram(program.text_buffer, PipelineKind::HF, labelLeaders(program));
    const std::string written = writeAssembly(source, program, report);

    EXPECT_NE(written.find("main:"), std::string::npos) << written;   // kept even though no branch uses it
    EXPECT_NE(written.find("DONE:"), std::string::npos) << written;
    EXPECT_NE(written.find("beq x1, x0, DONE"), std::string::npos) << written;
    EXPECT_EQ(written.find("L3"), std::string::npos) << written;      // nothing generated
    EXPECT_NE(written.find("# line 3: beq x1, x0, DONE"), std::string::npos) << written;
}

TEST(ReorderAsmWriter, GeneratedLabelOnlyWhereSourceHasNone)
{
    // beq at index 1 jumps by a number (+8 bytes) to index 3, which has no
    // label; END (index 4) is a real label. A source label named L3 forces
    // the generated prefix to change so the two can't collide.
    const std::string source =
        "L3:\naddi x1, x0, 1\nbeq x1, x0, 8\naddi x2, x0, 2\naddi x3, x0, 3\njal x0, END\nEND:\n";
    Kites::AssembledProgram program = assembleSource(source);
    ScheduleReport report = scheduleProgram(program.text_buffer, PipelineKind::HF, labelLeaders(program));
    const std::string written = writeAssembly(source, program, report);

    EXPECT_NE(written.find("L_3:"), std::string::npos) << written;
    EXPECT_NE(written.find("beq x1, x0, L_3"), std::string::npos) << written;
    EXPECT_NE(written.find("jal x0, END"), std::string::npos) << written;
    EXPECT_EQ(written.find("L_4"), std::string::npos) << written;
    roundTrip({"generated_label", source});
}

// =======================================================================
// Minimal-move suggestions.
// =======================================================================

namespace
{

// Report for one block whose new order lists these original indices.
ScheduleReport reportWithOrder(const std::vector<size_t> &newOrder)
{
    ScheduleReport report;
    report.words.assign(newOrder.size(), 0x13);
    for (size_t pos = 0; pos < newOrder.size(); ++pos)
    {
        if (newOrder[pos] != pos)
        {
            report.moves.push_back({newOrder[pos], pos});
        }
    }
    report.blocks.push_back({Block{0, newOrder.size(), false}, 0, 0});
    return report;
}

// Independent O(n^2) longest-increasing-subsequence length.
size_t lisLength(const std::vector<size_t> &seq)
{
    std::vector<size_t> best(seq.size(), 1);
    size_t longest = 0;
    for (size_t i = 0; i < seq.size(); ++i)
    {
        for (size_t j = 0; j < i; ++j)
        {
            if (seq[j] < seq[i])
            {
                best[i] = std::max(best[i], best[j] + 1);
            }
        }
        longest = std::max(longest, best[i]);
    }
    return longest;
}

// Every block: number of suggested moves == block size - LIS length; every
// anchor stays put; a hoist really is above an anchor it used to follow and
// a sink really is below one it used to precede.
void expectMinimalAndConsistent(const ScheduleReport &report, const std::string &name)
{
    const std::vector<size_t> original = originalIndexOf(report);
    std::vector<size_t> newPos(original.size());
    for (size_t pos = 0; pos < original.size(); ++pos)
    {
        newPos[original[pos]] = pos;
    }

    const std::vector<SuggestedMove> moves = suggestedMoves(report);
    for (size_t b = 0; b < report.blocks.size(); ++b)
    {
        const Block &block = report.blocks[b].block;
        const std::vector<size_t> seq(original.begin() + static_cast<std::ptrdiff_t>(block.begin),
                                      original.begin() + static_cast<std::ptrdiff_t>(block.end));
        const size_t inBlock = static_cast<size_t>(std::count_if(
            moves.begin(), moves.end(), [b](const SuggestedMove &m) { return m.blockIndex == b; }));
        EXPECT_EQ(inBlock, seq.size() - lisLength(seq)) << name << " block " << b;
    }
    for (const SuggestedMove &m : moves)
    {
        for (const SuggestedMove &other : moves)
        {
            EXPECT_NE(other.originalIndex, m.anchorOriginalIndex) << name << ": anchor also moves";
        }
        if (m.hoist)
        {
            EXPECT_GT(m.originalIndex, m.anchorOriginalIndex) << name;
            EXPECT_LT(newPos[m.originalIndex], newPos[m.anchorOriginalIndex]) << name;
        }
        else
        {
            EXPECT_LT(m.originalIndex, m.anchorOriginalIndex) << name;
            EXPECT_GT(newPos[m.originalIndex], newPos[m.anchorOriginalIndex]) << name;
        }
    }
}

} // namespace

TEST(ReorderAsmWriter, MinimalMoves_HandBuilt)
{
    EXPECT_TRUE(suggestedMoves(reportWithOrder({0, 1, 2, 3})).empty());

    // 0 2 3 1: only 1 moved -- it sank below 3.
    auto sink = suggestedMoves(reportWithOrder({0, 2, 3, 1}));
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink[0].originalIndex, 1u);
    EXPECT_FALSE(sink[0].hoist);
    EXPECT_EQ(sink[0].anchorOriginalIndex, 3u);

    // 3 0 1 2: only 3 moved -- it was hoisted above 0. (Old->new positions
    // would list all four instructions as moved.)
    auto hoist = suggestedMoves(reportWithOrder({3, 0, 1, 2}));
    ASSERT_EQ(hoist.size(), 1u);
    EXPECT_EQ(hoist[0].originalIndex, 3u);
    EXPECT_TRUE(hoist[0].hoist);
    EXPECT_EQ(hoist[0].anchorOriginalIndex, 0u);

    expectMinimalAndConsistent(reportWithOrder({4, 0, 3, 1, 2, 5}), "hand-built");
    expectMinimalAndConsistent(reportWithOrder({5, 4, 3, 2, 1, 0}), "reversed");
}

TEST(ReorderAsmWriter, MinimalMoves_LoopAndRandomPrograms)
{
    std::vector<NamedProgram> programs = loopPrograms();
    std::mt19937 rng(4242u);
    for (int t = 0; t < 100; ++t)
    {
        const int len = std::uniform_int_distribution<int>(10, 25)(rng);
        programs.push_back({"random#" + std::to_string(t), makeRandomBranchyProgram(rng, len)});
    }
    for (const NamedProgram &p : programs)
    {
        Kites::AssembledProgram program = assembleSource(p.source);
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            expectMinimalAndConsistent(
                scheduleProgram(program.text_buffer, kind, labelLeaders(program)), p.name);
        }
    }
}

TEST(ReorderAsmWriter, SuggestionsForInPlaceIncrementLoop)
{
    const NamedProgram loop = loopPrograms()[3];
    ASSERT_EQ(loop.name, "loop_inplace_inc_read_next");
    Kites::AssembledProgram program = assembleSource(loop.source);
    ScheduleReport report = scheduleProgram(program.text_buffer, PipelineKind::HNF, labelLeaders(program));
    const std::string text = writeSuggestions(loop.source, program, report, PipelineKind::HNF);

    EXPECT_LT(suggestedMoves(report).size(), report.moves.size()) << text;
    EXPECT_NE(text.find("hoist line 20 lw x25, 4(x20) above"), std::string::npos) << text;
    EXPECT_NE(text.find("All position changes, old -> new"), std::string::npos) << text;
}

// The two forms Kites' disassemble() gets wrong, fixed in disassembleForKites().
TEST(ReorderAsmWriter, DisassemblyFixesReassemble)
{
    const std::vector<std::string> lines = {"srai x9, x5, 63",  "srai x9, x5, 32",   "srai x9, x5, 1",
                                            "fcvt.s.w f1, x1",  "fcvt.s.wu f1, x1",  "fcvt.s.l f1, x1",
                                            "fcvt.s.lu f1, x1", "fcvt.d.w f2, x3",   "fcvt.d.wu f2, x3",
                                            "fcvt.d.l f2, x3",  "fcvt.d.lu f2, x3"};
    for (const std::string &line : lines)
    {
        const uint32_t word = assembleSource(line + "\n").text_buffer.at(0);
        EXPECT_EQ(disassembleForKites(word), line);
        EXPECT_EQ(assembleSource(disassembleForKites(word) + "\n").text_buffer.at(0), word) << line;
    }
}

// The base comes from lui, not la: on the rv5s VMs auipc leaves its
// destination at 0, so la never yields the data address there.
TEST(ReorderAsmWriter, DataSectionCopiedVerbatim)
{
    const std::string source =
        ".data\narr: .word 5, 6, 7, 8\n.text\n"
        "lui x10, 65536\nlw x5, 0(x10)\nlw x6, 4(x10)\nadd x7, x5, x5\nlw x8, 8(x10)\n"
        "add x9, x8, x6\nsw x9, 12(x10)\n";
    Kites::AssembledProgram program = assembleSource(source);
    ScheduleReport report = scheduleProgram(program.text_buffer, PipelineKind::HF, labelLeaders(program));
    const std::string written = writeAssembly(source, program, report);
    EXPECT_EQ(written.compare(0, 28, ".data\narr: .word 5, 6, 7, 8\n"), 0) << written;
    roundTrip({"data_section", source});
}

// Instructions whose disassembly is easy to get wrong: shifts with shamt >= 32,
// negative and extreme immediates, lui/auipc, li, nop, RV64 mulw.
// (addiw/sraiw/addw/subw are left out: the Kites assembler rejects them in source.)
TEST(ReorderAsmWriter, TrickyEncodingsRoundTrip)
{
    roundTrip({"tricky",
               "addi x5, x0, -2048\naddi x6, x0, 2047\nslli x7, x5, 40\nsrli x8, x5, 33\n"
               "srai x9, x5, 63\nsrai x13, x5, 1\nlui x14, 1048575\nlui x15, 1\n"
               "sw x5, -4(x15)\nlw x16, -4(x15)\nmulw x21, x5, x6\ndivu x22, x6, x5\n"
               "remu x23, x6, x5\nsltiu x24, x5, -1\nxori x25, x5, -1\nauipc x26, 0\n"
               "jal x1, NEXT\nNEXT:\nli x28, 0x12345678\naddi x0, x0, 0\nmulh x29, x5, x6\n"});
}

// =======================================================================
// Round trips.
// =======================================================================

TEST(ReorderAsmWriter, RoundTrip_AllExamplePrograms)
{
    const std::vector<NamedProgram> programs = examplePrograms();
    ASSERT_GE(programs.size(), 30u);
    for (const NamedProgram &p : programs)
    {
        roundTrip(p);
    }
    std::cout << "[ASMWRITER] round-tripped " << programs.size() << " example programs" << std::endl;
}

TEST(ReorderAsmWriter, RoundTrip_LoopPrograms)
{
    for (const NamedProgram &p : loopPrograms())
    {
        roundTrip(p);
    }
}

TEST(ReorderAsmWriter, RoundTrip_RandomForwardBranches)
{
    std::mt19937 rng(777u);
    int branches = 0, moved = 0;
    for (int t = 0; t < 100; ++t)
    {
        const int len = std::uniform_int_distribution<int>(10, 25)(rng);
        const std::string source = makeRandomBranchyProgram(rng, len);
        roundTrip({"random#" + std::to_string(t), source});

        Kites::AssembledProgram program = assembleSource(source);
        for (uint32_t w : program.text_buffer)
        {
            const DecodedInst d = decode(w);
            branches += (d.kind == Kind::Branch || d.kind == Kind::Jal) ? 1 : 0;
        }
        moved += static_cast<int>(
            scheduleProgram(program.text_buffer, PipelineKind::HNF, labelLeaders(program)).moves.size());
    }
    std::cout << "[ASMWRITER] 100 random programs: " << branches << " branches/jals, " << moved
              << " moved instructions (h_nf)" << std::endl;
}

// =======================================================================
// Files to show: build/reorder_out/<name>_<pipeline>.s and _suggestions.txt
// =======================================================================

TEST(ReorderAsmWriter, WriteOutputsForShowing)
{
    const fs::path outDir = kRepoRoot / "build" / "reorder_out";
    fs::remove_all(outDir);
    fs::create_directories(outDir);

    std::vector<NamedProgram> programs = hazardExamples();
    for (const NamedProgram &p : loopPrograms())
    {
        programs.push_back(p);
    }

    int files = 0;
    for (const NamedProgram &p : programs)
    {
        const std::string stem = p.name.substr(0, p.name.rfind(".s") == p.name.size() - 2
                                                      ? p.name.size() - 2
                                                      : p.name.size());
        Kites::AssembledProgram program = assembleSource(p.source);
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            ScheduleReport report = scheduleProgram(program.text_buffer, kind, labelLeaders(program));
            const std::string base = stem + "_" + pipelineName(kind);
            std::ofstream(outDir / (base + ".s")) << writeAssembly(p.source, program, report);
            std::ofstream(outDir / (base + "_suggestions.txt"))
                << writeSuggestions(p.source, program, report, kind);
            files += 2;
        }
    }
    std::cout << "[ASMWRITER] wrote " << files << " files to " << outDir.string() << std::endl;
    EXPECT_EQ(files, static_cast<int>(programs.size()) * 4);
}
