#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/asm_writer.h"
#include "reorder/program_runner.h"
#include "utils/utils.h"

// Demo walkthrough of the whole reorder pipeline on one program:
//   REORDER_DEMO_FILE=test/reorder_demo/quirk_imm5.s \
//     ./build/test/test/kites_test --gtest_filter=ReorderDemo.Walkthrough
// Without REORDER_DEMO_FILE it prints a usage line and passes, so the normal
// test suite is unaffected.

using namespace Kites::reorder;
namespace fs = std::filesystem;

namespace
{

const fs::path kRepoRoot = fs::path(__FILE__).parent_path().parent_path();
constexpr unsigned kCycleCap = 100000;

std::string reg(unsigned r) { return "x" + std::to_string(r); }

const char *kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Lui: return "lui";
    case Kind::Auipc: return "auipc";
    case Kind::Jal: return "jal";
    case Kind::Jalr: return "jalr";
    case Kind::Branch: return "branch";
    case Kind::Load: return "load";
    case Kind::Store: return "store";
    case Kind::OpImm: return "op-imm";
    case Kind::Op: return "op";
    case Kind::Ecall: return "ecall";
    case Kind::Other: return "other";
    }
    return "?";
}

const char *barrierWhat(const DecodedInst &d)
{
    if (d.is_branch) return "branch";
    if (d.is_jump) return "jump";
    if (d.kind == Kind::Ecall) return "ecall";
    if (d.kind == Kind::Auipc) return "auipc (PC-relative)";
    return "unknown opcode";
}

const char *pipelineLabel(PipelineKind kind) { return kind == PipelineKind::HF ? "rv5-h-f" : "rv5-h-nf"; }
const char *pipelineFile(PipelineKind kind) { return kind == PipelineKind::HF ? "h_f" : "h_nf"; }

std::string byteRange(const DecodedInst &d)
{
    return "[" + std::to_string(d.mem_offset) + "," + std::to_string(d.mem_offset + d.mem_size) + ")";
}

void section(const std::string &title)
{
    std::cout << "\n==== " << title << " ====\n";
}

// ---------------------------------------------------------------------
// (a) one decoded instruction
// ---------------------------------------------------------------------
void printDecoded(size_t index, const DecodedInst &d, const Kites::AssembledProgram &program,
                  const std::vector<std::string> &lines)
{
    const unsigned line = sourceLineOf(program, index);
    std::cout << "[" << std::setw(2) << index << "] line " << std::setw(3) << line << "  "
              << std::left << std::setw(24) << disassembleForKites(d.word) << std::right
              << "  source: " << sourceTextOf(lines, line) << "\n";

    std::cout << "      kind " << kindName(d.kind);
    if (d.writes_rd)
    {
        std::cout << ", writes rd = " << reg(d.rd) << (d.rd == 0 ? " (x0: write discarded)" : "");
    }
    else
    {
        std::cout << ", writes no register";
    }
    std::cout << ", true reads: ";
    std::vector<std::string> reads;
    if (d.reads_rs1) reads.push_back(reg(d.rs1));
    if (d.reads_rs2) reads.push_back(reg(d.rs2));
    if (reads.empty()) std::cout << "none";
    for (size_t k = 0; k < reads.size(); ++k) std::cout << (k ? ", " : "") << reads[k];
    std::cout << ", barrier: " << (d.is_barrier ? std::string("yes (") + barrierWhat(d) + ")" : "no")
              << "\n";

    std::cout << "      raw rs1 = " << reg(d.rs1)
              << (d.reads_rs1 ? " (real read)" : " (immediate bits, not a real read)") << "; raw rs2 = "
              << reg(d.rs2) << (d.reads_rs2 ? " (real read)" : " (immediate bits, not a real read)");
    if (d.is_load || d.is_store)
    {
        std::cout << "; memory: base " << reg(d.rs1) << ", bytes " << byteRange(d);
    }
    std::cout << "\n";
}

// ---------------------------------------------------------------------
// (c) why j must stay after i, recomputed from the decoded fields
// ---------------------------------------------------------------------
struct EdgeExplanation
{
    std::vector<std::string> reasons;
    bool keepsEdge = false;
};

EdgeExplanation explainPair(const std::vector<DecodedInst> &insts, size_t i, size_t j, size_t blockBegin,
                            const Kites::AssembledProgram &program)
{
    const DecodedInst &a = insts[i];
    const DecodedInst &b = insts[j];
    EdgeExplanation e;

    const bool barrier = a.is_barrier || b.is_barrier;
    if (barrier)
    {
        const DecodedInst &which = a.is_barrier ? a : b;
        e.reasons.push_back(std::string("BARRIER (") + disassembleForKites(which.word) + " is a " +
                            barrierWhat(which) + ")");
    }

    const bool raw = writesReg(a) && ((b.reads_rs1 && b.rs1 == a.rd) || (b.reads_rs2 && b.rs2 == a.rd));
    const bool war = writesReg(b) && ((a.reads_rs1 && a.rs1 == b.rd) || (a.reads_rs2 && a.rs2 == b.rd));
    const bool waw = writesReg(a) && writesReg(b) && a.rd == b.rd;
    if (raw) e.reasons.push_back("RAW " + reg(a.rd));
    if (war) e.reasons.push_back("WAR " + reg(b.rd));
    if (waw) e.reasons.push_back("WAW " + reg(a.rd));

    const bool mem = (a.is_load || a.is_store) && (b.is_load || b.is_store) && (a.is_store || b.is_store);
    bool memKeeps = false;
    if (mem)
    {
        if (barrier || raw || war || waw)
        {
            e.reasons.push_back("MEM (already ordered by the reasons above)");
            memKeeps = true;
        }
        else if (canDropMemoryEdge(insts, i, j))
        {
            e.reasons.push_back("MEM dropped by disambiguation: same base " + reg(a.rs1) + ", bytes " +
                                byteRange(a) + " and " + byteRange(b) + " don't overlap");
        }
        else
        {
            memKeeps = true;
            std::string why;
            if (a.rs1 != b.rs1)
            {
                why = "different bases " + reg(a.rs1) + " and " + reg(b.rs1) + ", addresses can't be compared";
            }
            else
            {
                for (size_t k = i; k <= j && why.empty(); ++k)
                {
                    if (writesReg(insts[k]) && insts[k].rd == a.rs1)
                    {
                        why = "base " + reg(a.rs1) + " is rewritten by [" + std::to_string(blockBegin + k) +
                              "] (line " + std::to_string(sourceLineOf(program, blockBegin + k)) + ")";
                    }
                }
                if (why.empty())
                {
                    why = "same base " + reg(a.rs1) + ", bytes " + byteRange(a) + " and " + byteRange(b) +
                          " overlap";
                }
            }
            e.reasons.push_back("MEM kept: " + why);
        }
    }

    e.keepsEdge = barrier || raw || war || waw || memKeeps;
    return e;
}

// ---------------------------------------------------------------------
// (d) predicted stalls charged right before each instruction issues,
// threading the head context between blocks exactly like scheduleProgram.
// ---------------------------------------------------------------------
std::vector<int> stallsPerInstruction(const std::vector<uint32_t> &words, const std::vector<Block> &blocks,
                                      PipelineKind kind)
{
    std::vector<int> per(words.size(), 0);
    DecodedInst prev1{}, prev2{};
    bool have1 = false, have2 = false;
    for (const Block &block : blocks)
    {
        HeadContext ctx;
        if (have1) ctx.prev1 = &prev1;
        if (have2) ctx.prev2 = &prev2;

        std::vector<DecodedInst> insts;
        int soFar = 0;
        for (size_t k = block.begin; k < block.end; ++k)
        {
            insts.push_back(decode(words[k]));
            const int now = predictStalls(insts, kind, ctx);
            per[k] = now - soFar;
            soFar = now;
        }

        if (insts.size() >= 2)
        {
            prev2 = insts[insts.size() - 2];
            have2 = true;
        }
        else
        {
            prev2 = prev1;
            have2 = have1;
        }
        prev1 = insts.back();
        have1 = true;
    }
    return per;
}

int printStallTable(const char *title, const std::vector<uint32_t> &words, const std::vector<size_t> &original,
                    const std::vector<int> &per, const Kites::AssembledProgram &program,
                    const std::vector<std::string> &lines)
{
    std::cout << title << "\n  pos  orig  line  instruction               stalls before it\n";
    int total = 0;
    for (size_t pos = 0; pos < words.size(); ++pos)
    {
        const unsigned line = sourceLineOf(program, original[pos]);
        std::cout << "  " << std::setw(3) << pos << "  " << std::setw(4) << original[pos] << "  "
                  << std::setw(4) << line << "  " << std::left << std::setw(26)
                  << disassembleForKites(words[pos]) << std::right << std::setw(4) << per[pos]
                  << (per[pos] > 0 ? "   <- " + sourceTextOf(lines, line) : "") << "\n";
        total += per[pos];
    }
    std::cout << "  total predicted stalls: " << total << "\n";
    return total;
}

// ---------------------------------------------------------------------
// (f) real VM runs
// ---------------------------------------------------------------------
template <typename VmType>
void printVmComparison(PipelineKind kind, const Kites::AssembledProgram &program,
                       const std::vector<uint32_t> &scheduledWords, uint64_t stackTop)
{
    const Kites::RunResult a = Kites::runProgram<VmType>(program, {}, kCycleCap, 64, stackTop, 64);
    const Kites::RunResult b =
        Kites::runProgram<VmType>(Kites::withText(program, scheduledWords), {}, kCycleCap, 64, stackTop, 64);

    auto cpi = [](const Kites::RunResult &r)
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(3)
          << (r.retired ? static_cast<double>(r.cycles) / r.retired : 0.0);
        return s.str();
    };
    auto row = [](const char *name, const std::string &x, const std::string &y)
    { std::cout << "    " << std::left << std::setw(10) << name << std::right << std::setw(10) << x
                << std::setw(12) << y << "\n"; };

    std::cout << "  " << pipelineLabel(kind) << "       original   reordered\n";
    row("cycles", std::to_string(a.cycles), std::to_string(b.cycles));
    row("stalls", std::to_string(a.stalls), std::to_string(b.stalls));
    row("retired", std::to_string(a.retired), std::to_string(b.retired));
    row("CPI", cpi(a), cpi(b));
    row("hitCap", a.hitCap ? "yes" : "no", b.hitCap ? "yes" : "no");

    std::cout << "    sameState: ";
    if (Kites::sameState(a, b))
    {
        std::cout << "equivalent\n";
    }
    else if (a.hitCap || b.hitCap)
    {
        std::cout << "not decided (a run hit the " << kCycleCap << "-cycle cap)\n";
    }
    else
    {
        std::cout << "NOT equivalent\n";
    }
}

} // namespace

TEST(ReorderDemo, Walkthrough)
{
    const char *env = std::getenv("REORDER_DEMO_FILE");
    if (env == nullptr || *env == '\0')
    {
        std::cout << "ReorderDemo: set REORDER_DEMO_FILE=<file.s> to run the walkthrough, e.g. "
                     "REORDER_DEMO_FILE=test/reorder_demo/quirk_imm5.s ./build/test/test/kites_test "
                     "--gtest_filter=ReorderDemo.Walkthrough"
                  << std::endl;
        return;
    }

    fs::path path(env);
    if (!fs::exists(path) && path.is_relative() && fs::exists(kRepoRoot / path))
    {
        path = kRepoRoot / path;
    }
    std::ifstream in(path);
    ASSERT_TRUE(in) << "cannot read " << path;
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    const std::string name = path.stem().string();

    Kites::setupVmStateDirectory();
    Kites::AssembledProgram program;
    try
    {
        std::istringstream stream(source);
        program = Kites::assemble(stream);
    }
    catch (const std::exception &e)
    {
        FAIL() << path << " does not assemble: " << e.what();
    }

    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<std::string> lines = splitLines(source);
    const std::vector<size_t> leaders = labelLeaders(program);
    const std::vector<Block> blocks = findBlocks(words, leaders);
    const auto labels = textLabelsByIndex(program);

    std::cout << "\n######## Reorder demo: " << path.string() << " (" << words.size() << " instructions, "
              << blocks.size() << (blocks.size() == 1 ? " block" : " blocks") << ") ########\n";

    // (a)
    section("(a) Decoded instructions");
    std::cout << "True reads drive the dependency graph (legality). The hazard unit compares the RAW\n"
                 "rs1/rs2 bit fields of every instruction, so the cost model uses those (the quirk).\n";
    for (size_t i = 0; i < words.size(); ++i)
    {
        printDecoded(i, decode(words[i]), program, lines);
    }

    // (b)
    section("(b) Basic blocks");
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const Block &block = blocks[b];
        std::cout << "block " << b << ": instructions " << block.begin << "-" << (block.end - 1)
                  << ", lines " << sourceLineOf(program, block.begin) << "-"
                  << sourceLineOf(program, block.end - 1);
        auto it = labels.find(block.begin);
        if (it != labels.end())
        {
            std::cout << ", starts at label";
            for (const std::string &label : it->second) std::cout << " " << label;
        }
        std::cout << (block.hasTerminator ? ", ends with " + disassembleForKites(words[block.end - 1]) +
                                                " (stays last)"
                                          : ", no terminator")
                  << "\n";
    }

    // (c)
    section("(c) Dependency edges per block (j must stay after i)");
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const Block &block = blocks[b];
        const DepGraph g = buildDepGraph(words, block);
        std::vector<DecodedInst> insts;
        for (size_t k = block.begin; k < block.end; ++k) insts.push_back(decode(words[k]));

        std::cout << "block " << b << " (instructions " << block.begin << "-" << (block.end - 1) << "):\n";
        bool any = false;
        for (size_t i = 0; i < insts.size(); ++i)
        {
            for (size_t j = i + 1; j < insts.size(); ++j)
            {
                const EdgeExplanation e = explainPair(insts, i, j, block.begin, program);
                if (e.reasons.empty()) continue;
                any = true;
                EXPECT_EQ(e.keepsEdge, g.hasEdge(i, j)) << "recomputed edge disagrees with buildDepGraph";
                std::cout << "  [" << block.begin + i << "] " << std::left << std::setw(20)
                          << disassembleForKites(insts[i].word) << " -> [" << block.begin + j << "] "
                          << std::setw(20) << disassembleForKites(insts[j].word) << std::right
                          << (e.keepsEdge ? "  edge:    " : "  no edge: ");
                for (size_t r = 0; r < e.reasons.size(); ++r) std::cout << (r ? "; " : "") << e.reasons[r];
                std::cout << "\n";
            }
        }
        if (!any) std::cout << "  (no dependencies: any order is legal)\n";
    }

    // (d) + (e) per pipeline
    std::vector<uint32_t> scheduled[2];
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        const ScheduleReport report = scheduleProgram(words, kind, leaders);
        scheduled[kind == PipelineKind::HF ? 0 : 1] = report.words;
        const std::vector<size_t> original = originalIndexOf(report);
        std::vector<size_t> identity(words.size());
        for (size_t k = 0; k < identity.size(); ++k) identity[k] = k;

        section(std::string("(d) Predicted stalls on ") + pipelineLabel(kind));
        const int before = printStallTable("original order:", words, identity,
                                           stallsPerInstruction(words, blocks, kind), program, lines);
        const int after = printStallTable("scheduled order:", report.words, original,
                                          stallsPerInstruction(report.words, blocks, kind), program, lines);
        EXPECT_EQ(before, report.stallsBefore);
        EXPECT_EQ(after, report.stallsAfter);

        section(std::string("(e) Suggestions and reordered .s for ") + pipelineLabel(kind));
        std::cout << writeSuggestions(source, program, report, kind) << "\n";
        const std::string assembly = writeAssembly(source, program, report);
        std::cout << assembly;
        const fs::path outDir = kRepoRoot / "build" / "reorder_out";
        fs::create_directories(outDir);
        const fs::path outFile = outDir / ("demo_" + name + "_" + pipelineFile(kind) + ".s");
        std::ofstream(outFile) << assembly;
        std::cout << "(saved to " << outFile.string() << ")\n";
    }

    // (f)
    section("(f) Real VM runs (runProgram, cap " + std::to_string(kCycleCap) + " cycles)");
    uint64_t stackTop = 0;
    {
        Kites::reorder_detail::CoutSilencer quiet;
        auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
        vm->LoadProgram(program);
        stackTop = vm->registers_.ReadGpr(2);
    }
    printVmComparison<Kites::RV5StageProcessorHF>(PipelineKind::HF, program, scheduled[0], stackTop);
    printVmComparison<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, program, scheduled[1], stackTop);
    std::cout << "  sameState compares x0-x31, 64 words from the data-section start and 64 words below sp;\n"
                 "  memory elsewhere (e.g. absolute addresses like 1000) is not compared.\n"
                 "  VM stalls are dynamic (every loop iteration counts); (d) counts each instruction once.\n";
}
