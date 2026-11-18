#pragma once
// Benchmark evaluation for the reordering project (Stage 3):
//  - the program header every benchmark declares (name, category, source, inputs),
//  - the validity filter (assembles, I/M only, no input syscalls, exit ecall last, no stack,
//    data fits the compared window, finishes within the cycle cap, doesn't overwrite its code),
//  - near-duplicate removal,
//  - the batch run on rv5-h-f and rv5-h-nf with CSV output,
//  - classification of the blocks whose stalls the scheduler could not remove.
//
// Qt-free. Calls A's library (scheduler, cost model, dependency graph) without changing it.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "assembler/assembler.h"
#include "reorder/asm_writer.h"
#include "reorder/block_finder.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/dep_graph.h"
#include "reorder/reorder_report.h"
#include "reorder/scheduler.h"
#include "reorder/vm_safety_net.h"

namespace Kites::bench_eval
{
using reorder::PipelineKind;
using reorder::ScheduleReport;

constexpr unsigned kBenchCycleCap = 200000;

inline const std::vector<std::string> &benchCategories()
{
    static const std::vector<std::string> categories = {"hazard", "loop", "aliasing", "chain",
                                                        "mixed"};
    return categories;
}

// ---------------------------------------------------------------------------------------------
// Header: leading "# key: value" comment lines.
//   # name: array_sum
//   # category: loop            (hazard | loop | aliasing | chain | mixed)
//   # source: llm               (llm | reference)
//   # inputs: none              (or a seed: x10=0x10000000,mem:0x10000000=7)
//   # description: ...
// ---------------------------------------------------------------------------------------------

struct ProgramHeader
{
    std::map<std::string, std::string> fields;

    std::string get(const std::string &key) const
    {
        auto it = fields.find(key);
        return it == fields.end() ? "" : it->second;
    }
    bool has(const std::string &key) const { return fields.count(key) != 0; }
};

inline ProgramHeader parseProgramHeader(const std::string &source)
{
    ProgramHeader header;
    for (const std::string &raw : reorder::splitLines(source))
    {
        const std::string line = reorder_report::trimSpaces(raw);
        if (line.empty())
        {
            continue;
        }
        if (line[0] != '#')
        {
            break; // the header is the leading comment block
        }
        const std::string body = reorder_report::trimSpaces(line.substr(1));
        const size_t colon = body.find(':');
        if (colon == std::string::npos || colon == 0)
        {
            continue;
        }
        std::string key = reorder_report::trimSpaces(body.substr(0, colon));
        if (key.find(' ') != std::string::npos)
        {
            continue; // prose comment, not a key
        }
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!header.has(key))
        {
            header.fields[key] = reorder_report::trimSpaces(body.substr(colon + 1));
        }
    }
    return header;
}

// ---------------------------------------------------------------------------------------------
// Validity filter
// ---------------------------------------------------------------------------------------------

inline bool isExitEcall(uint32_t word) { return word == 0x00000073u; }

// Base integer (RV64I) and M only: no CSR / fence / ebreak / atomics / floating point.
inline bool isIntegerOrMulDiv(uint32_t word)
{
    const uint32_t opcode = word & 0x7F;
    const uint32_t funct7 = word >> 25;
    switch (opcode)
    {
    case 0x37: case 0x17: case 0x6F: case 0x67: case 0x63:
    case 0x03: case 0x23: case 0x13: case 0x1B:
        return true;
    case 0x33: case 0x3B:
        return funct7 == 0x00 || funct7 == 0x20 || funct7 == 0x01; // base, sub/sra, M
    case 0x73:
        return isExitEcall(word);
    default:
        return false;
    }
}

inline size_t dataBytes(const AssembledProgram &program)
{
    size_t bytes = 0;
    for (const auto &item : program.data_buffer)
    {
        std::visit(
            [&bytes](const auto &value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    bytes += value.size() + 1;
                }
                else
                {
                    bytes += sizeof(T);
                }
            },
            item);
    }
    const uint64_t dataStart = vm_config::config.getDataSectionStart();
    for (const auto &[name, sym] : program.symbol_table)
    {
        if (sym.isData && sym.address >= dataStart)
        {
            bytes = std::max<size_t>(bytes, static_cast<size_t>(sym.address - dataStart) + 1);
        }
    }
    return bytes;
}

// The Kites assembler never returns on a label named after a section directive (found while
// building the benchmark set: `data:`, `bss:` and `section:` loop forever; `text:` is rejected
// normally). Since that hang is in-process, it is caught here, before assembling.
inline std::string assemblerHangLabel(const std::string &source)
{
    static const std::set<std::string> hangNames = {"data", "bss", "section"};
    for (const std::string &raw : reorder::splitLines(source))
    {
        const std::string code = reorder::stripComment(raw);
        const size_t colon = code.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        const std::string label = reorder_report::trimSpaces(code.substr(0, colon));
        if (hangNames.count(label))
        {
            return label;
        }
    }
    return "";
}

// Checks that need only the assembled program and its header. Each failure is one reason.
inline std::vector<std::string> staticValidityProblems(const AssembledProgram &program,
                                                       const ProgramHeader &header, Seed &seedOut)
{
    std::vector<std::string> problems;
    const std::vector<uint32_t> &words = program.text_buffer;

    const std::string category = header.get("category");
    const auto &categories = benchCategories();
    if (std::find(categories.begin(), categories.end(), category) == categories.end())
    {
        problems.push_back("header: category must be one of hazard/loop/aliasing/chain/mixed");
    }
    const std::string source = header.get("source");
    if (source != "llm" && source != "reference")
    {
        problems.push_back("header: source must be llm or reference");
    }
    if (!header.has("inputs"))
    {
        problems.push_back("header: missing '# inputs:' line (none, or a seed)");
    }
    else if (header.get("inputs") != "none")
    {
        try
        {
            seedOut = reorder_report::parseSeed(header.get("inputs"));
        }
        catch (const std::exception &e)
        {
            problems.push_back(std::string("header: bad inputs: ") + e.what());
        }
    }

    if (words.empty())
    {
        problems.push_back("no instructions");
        return problems;
    }

    for (size_t i = 0; i < words.size(); ++i)
    {
        if (!isIntegerOrMulDiv(words[i]))
        {
            problems.push_back("instruction " + std::to_string(i) + " is not I/M (" +
                               reorder::disassembleForKites(words[i]) + ")");
            break;
        }
    }

    size_t ecallCount = 0;
    for (uint32_t w : words)
    {
        ecallCount += isExitEcall(w) ? 1 : 0;
    }
    if (!isExitEcall(words.back()))
    {
        problems.push_back("does not end with the exit ecall");
    }
    if (ecallCount > 1 || (ecallCount == 1 && !isExitEcall(words.back())))
    {
        problems.push_back("ecall used before the end (only one exit ecall, last, is allowed)");
    }
    if (isExitEcall(words.back()))
    {
        // a7 must be set to 10 (exit) in the block that leads into the ecall; any other value is
        // an input or print syscall.
        const std::vector<reorder::Block> blocks =
            reorder::findBlocks(words, reorder::labelLeaders(program));
        const size_t ecallIndex = words.size() - 1;
        size_t searchBegin = 0;
        for (const reorder::Block &b : blocks)
        {
            if (b.begin < ecallIndex && ecallIndex - 1 < b.end)
            {
                searchBegin = b.begin;
            }
        }
        bool found = false;
        for (size_t k = ecallIndex; k-- > searchBegin;)
        {
            const reorder::DecodedInst d = reorder::decode(words[k]);
            if (reorder::writesReg(d) && d.rd == 17)
            {
                const bool isLiTen = (words[k] & 0x7F) == 0x13 && ((words[k] >> 12) & 7) == 0 &&
                                     d.rs1 == 0 && (words[k] >> 20) == 10;
                if (!isLiTen)
                {
                    problems.push_back("ecall is not exit: a7 is not set to 10 (input/print "
                                       "syscalls are not allowed)");
                }
                found = true;
                break;
            }
        }
        if (!found && ecallIndex > 0)
        {
            problems.push_back("exit ecall without 'li a7, 10' in the same block before it");
        }
        // rv5-h-f / rv5-h-nf never call handle_syscall(): ecall is a no-op there and a program
        // ends when it runs off the end of the text. With the ecall last and nothing jumping past
        // it, finishing within the cycle cap means the exit ecall was reached.
        for (size_t k = 0; k < words.size(); ++k)
        {
            int64_t target = 0;
            if (reorder::directTarget(words[k], k, target) &&
                target >= static_cast<int64_t>(words.size()))
            {
                problems.push_back("a branch or jump skips the exit ecall (target past the end)");
                break;
            }
        }
    }

    for (uint32_t w : words)
    {
        const reorder::DecodedInst d = reorder::decode(w);
        const bool readsSp = (d.reads_rs1 && d.rs1 == 2) || (d.reads_rs2 && d.rs2 == 2);
        const bool writesSp = reorder::writesReg(d) && d.rd == 2;
        if (readsSp || writesSp)
        {
            problems.push_back("uses sp (x2); sp is not initialised, keep data in .data");
            break;
        }
    }

    if (dataBytes(program) > 4 * reorder_report::kDataWindowWords)
    {
        problems.push_back("data section is larger than the compared window (" +
                           std::to_string(4 * reorder_report::kDataWindowWords) + " bytes)");
    }
    return problems;
}

// Checks that need a run of the original program on both VMs.
inline std::vector<std::string> dynamicValidityProblems(const RunResult &hf, const RunResult &hnf)
{
    std::vector<std::string> problems;
    if (hf.hitCap || hnf.hitCap)
    {
        problems.push_back("does not finish within the cycle cap (" +
                           std::to_string(kBenchCycleCap) + ")");
        return problems;
    }
    if (!hf.textIntact || !hnf.textIntact)
    {
        problems.push_back("overwrites its own instructions (store to a low address)");
    }
    if (!sameState(hf, hnf))
    {
        problems.push_back("rv5-h-f and rv5-h-nf end in different states");
    }
    return problems;
}

// ---------------------------------------------------------------------------------------------
// Near-duplicates: same instruction shape, ignoring registers and immediates.
// ---------------------------------------------------------------------------------------------

inline std::vector<std::string> mnemonicSequence(const std::vector<uint32_t> &words)
{
    std::vector<std::string> mnemonics;
    for (uint32_t w : words)
    {
        const std::string text = reorder::disassembleForKites(w);
        mnemonics.push_back(text.substr(0, text.find(' ')));
    }
    return mnemonics;
}

// Jaccard similarity of 4-instruction shingles; 1.0 for identical sequences.
inline double shapeSimilarity(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    if (a == b)
    {
        return 1.0;
    }
    const size_t k = 4;
    if (a.size() < 8 || b.size() < 8)
    {
        return 0.0; // short programs count as duplicates only when identical
    }
    auto shingles = [k](const std::vector<std::string> &seq)
    {
        std::set<std::string> out;
        for (size_t i = 0; i + k <= seq.size(); ++i)
        {
            std::string s;
            for (size_t j = i; j < i + k; ++j)
            {
                s += seq[j] + ' ';
            }
            out.insert(s);
        }
        return out;
    };
    const std::set<std::string> sa = shingles(a), sb = shingles(b);
    size_t common = 0;
    for (const std::string &s : sa)
    {
        common += sb.count(s);
    }
    const size_t unionSize = sa.size() + sb.size() - common;
    return unionSize == 0 ? 1.0 : static_cast<double>(common) / static_cast<double>(unionSize);
}

constexpr double kDuplicateThreshold = 0.85;

// ---------------------------------------------------------------------------------------------
// Why could the scheduler not remove a block's remaining stalls?
// ---------------------------------------------------------------------------------------------

enum class StallReason
{
    // Decided from the dependency graph, only for blocks whose remaining stalls are proven optimal:
    BlockBoundary,      // the producer is before the block, the stall is in the next block's first
                        // instructions (the tail), or the block is too short to hold a filler
    DependencyChain,    // every instruction not already filling a gap is on one chain
    NothingIndependent, // nothing can go between producer and consumer, even ignoring memory, or
                        // the instructions that could are already filling other gaps
    MemoryOrdering,     // a filler would exist if possibly-aliasing loads/stores could swap
    FalseStall,         // the hazard unit read an immediate's bits as a register (no real dependence)
    // From the optimizer (scheduleBlockOptimalDetailed with the scheduler's own scoring):
    OtherNotProvenOptimal,  // the search did not prove the block optimal (node cap, or more than
                            // kMaxOptimalBlockSize instructions), so the reason is not claimed
    SchedulerMissedOptimum, // the search found an order with a lower score than the schedule
};

inline const char *stallReasonName(StallReason reason)
{
    switch (reason)
    {
    case StallReason::BlockBoundary: return "block_boundary";
    case StallReason::DependencyChain: return "dependency_chain";
    case StallReason::NothingIndependent: return "nothing_independent";
    case StallReason::MemoryOrdering: return "memory_ordering";
    case StallReason::FalseStall: return "false_stall_immediate";
    case StallReason::OtherNotProvenOptimal: return "other_not_proven_optimal";
    case StallReason::SchedulerMissedOptimum: return "scheduler_missed_optimum";
    }
    return "?";
}

// One block with a predicted stall score before or after scheduling. Two units, as in
// reorder::BlockStalls:
//  - stallsBefore/stallsAfter: plain predicted stalls (one context, no weights, no tail);
//  - scoreBefore/scoreAfter: the weighted objective the scheduler minimizes (ContextMode::
//    BlockEntries): stalls under each way into the block times that entry edge's weight (loop
//    edges count kDefaultLoopWeight), plus the weighted stalls the block causes in the next
//    block's first instructions. For a straight-line block entered only at program start the two
//    are equal.
// Status, the reason split and the optimizer comparison use the score (what the scheduler and the
// optimizer minimize); the plain stalls are reported alongside.
struct BlockOutcome
{
    reorder::Block block{};
    unsigned firstLine = 0;
    unsigned lastLine = 0;
    int stallsBefore = 0; // plain
    int stallsAfter = 0;
    int scoreBefore = 0;  // weighted
    int scoreAfter = 0;
    std::string status;   // by score: improved | partial | unimproved | worse (a stall added at the
                          // head because the block before it was reordered)
    bool hasReason = false;
    StallReason reason = StallReason::NothingIndependent; // reason for most of the remaining score
    std::map<StallReason, int> stallCyclesByReason;      // the remaining score, split by reason

    // From scheduleBlockOptimalDetailed() with the scheduler's own scoring (blocks with a score
    // left only), so both are in score units like scoreAfter.
    bool searched = false;
    int optimalScore = -1;
    int greedyScore = -1;
    reorder::OptimizerMethod method = reorder::OptimizerMethod::NotSearched;
    bool proven = false;
};

// reach[i][j]: instruction i must come before j (a path in the dependency graph).
inline std::vector<std::vector<bool>> reachability(const reorder::DepGraph &g)
{
    std::vector<std::vector<bool>> reach(g.size, std::vector<bool>(g.size, false));
    for (size_t i = g.size; i-- > 0;)
    {
        for (size_t j : g.succ[i])
        {
            reach[i][j] = true;
            for (size_t k = 0; k < g.size; ++k)
            {
                if (reach[j][k])
                {
                    reach[i][k] = true;
                }
            }
        }
    }
    return reach;
}

// The same graph without the edges that only memory ordering forces.
inline reorder::DepGraph withoutMemoryEdges(const reorder::DepGraph &g,
                                            const std::vector<reorder::DecodedInst> &insts)
{
    reorder::DepGraph out;
    out.size = g.size;
    out.succ.assign(g.size, {});
    out.pred.assign(g.size, {});
    for (size_t i = 0; i < g.size; ++i)
    {
        for (size_t j : g.succ[i])
        {
            if (!reorder::linkedOnlyByMemory(insts[i], insts[j]))
            {
                out.succ[i].push_back(j);
                out.pred[j].push_back(i);
            }
        }
    }
    return out;
}

// Scheduled positions that already fill a gap: they sit between a producer and a consumer that
// would stall without them (h_f: a load and its use; h_nf: any true dependence up to 3 apart).
inline std::vector<bool> busyFillers(const std::vector<reorder::DecodedInst> &scheduled,
                                     PipelineKind kind)
{
    std::vector<bool> busy(scheduled.size(), false);
    for (size_t a = 0; a < scheduled.size(); ++a)
    {
        const reorder::DecodedInst &p = scheduled[a];
        if (!reorder::writesReg(p) || (kind == PipelineKind::HF && !p.is_load))
        {
            continue;
        }
        const size_t maxGap = kind == PipelineKind::HF ? 2 : 3;
        for (size_t b = a + 2; b < scheduled.size() && b - a <= maxGap; ++b)
        {
            const reorder::DecodedInst &c = scheduled[b];
            if ((c.reads_rs1 && c.rs1 == p.rd) || (c.reads_rs2 && c.rs2 == p.rd))
            {
                for (size_t q = a + 1; q < b; ++q)
                {
                    busy[q] = true;
                }
            }
        }
    }
    return busy;
}

// Is there a free instruction x (not p or c, not a barrier, not pinned, not already filling
// another gap) that is independent of both p and c, so it could go between them? Indices are
// original-local; busyOriginal is indexed the same way.
inline bool fillerExists(const std::vector<std::vector<bool>> &reach,
                         const std::vector<reorder::DecodedInst> &insts,
                         const std::vector<bool> &busyOriginal, size_t p, size_t c)
{
    for (size_t x = 0; x < insts.size(); ++x)
    {
        if (x == p || x == c || insts[x].is_barrier || insts[x].is_pinned || busyOriginal[x])
        {
            continue;
        }
        // independent of both: no dependence path to or from p or c (otherwise moving x into the
        // gap would just make x stall, or make c stall on x)
        if (!reach[x][p] && !reach[p][x] && !reach[x][c] && !reach[c][x])
        {
            return true;
        }
    }
    return false;
}

// The BlockScoring scheduleProgram() (ContextMode::BlockEntries) used for block b: the
// fall-through head starts from the two words above the block in the SCHEDULED program, the
// clean head from an empty window, and the tail is the next block's first two original
// instructions. Mirrors scoringFrom() in scheduler.h; the test
// ReorderBenchEval.RebuiltScoringMatchesScheduler checks they agree on every benchmark block.
inline reorder::BlockScoring schedulerScoring(const std::vector<uint32_t> &originalWords,
                                              const std::vector<uint32_t> &scheduledWords,
                                              const std::vector<reorder::Block> &blocks,
                                              const std::vector<reorder::BlockEntry> &entries,
                                              size_t b,
                                              reorder::HazardModel model = reorder::HazardModel::Exact)
{
    const reorder::Block &block = blocks[b];
    reorder::DecodedInst prev1{}, prev2{};
    reorder::HeadContext ctx;
    if (block.begin >= 1)
    {
        prev1 = reorder::decode(scheduledWords[block.begin - 1]);
        ctx.prev1 = &prev1;
    }
    if (block.begin >= 2)
    {
        prev2 = reorder::decode(scheduledWords[block.begin - 2]);
        ctx.prev2 = &prev2;
    }
    const reorder::BlockEntry &entry = entries[b];
    reorder::BlockScoring scoring;
    scoring.model = model;
    if (entry.fallThrough)
    {
        scoring.heads.push_back(reorder::ScoredHead{reorder::windowFrom(ctx), entry.fallThroughWeight});
    }
    if (entry.clean || !entry.fallThrough)
    {
        scoring.heads.push_back(reorder::ScoredHead{reorder::WindowState{}, entry.clean ? entry.cleanWeight : 1});
    }
    if (b + 1 < blocks.size() && entries[b + 1].fallThrough)
    {
        const reorder::Block &next = blocks[b + 1];
        for (size_t k = next.begin; k < next.end && k < next.begin + 2; ++k)
        {
            scoring.tail.push_back(reorder::decode(originalWords[k]));
        }
        scoring.tailWeight = entries[b + 1].fallThroughWeight;
    }
    return scoring;
}

// Why does the stall at scheduled position `stalled` remain, in a block whose remaining stalls
// the optimizer proved optimal? Decided from the dependency graph alone.
inline StallReason graphReason(const std::vector<reorder::DecodedInst> &originalInsts,
                               const reorder::DepGraph &graph,
                               const std::vector<reorder::DecodedInst> &scheduledInsts,
                               const std::vector<size_t> &originalLocalOf, size_t stalled,
                               PipelineKind kind)
{
    const reorder::DecodedInst &c = scheduledInsts[stalled];
    // The hazard unit compares raw rs1/rs2 fields with the rd of the 1 or 2 instructions ahead.
    size_t producer = stalled;
    for (size_t back = 1; back <= 2 && back <= stalled; ++back)
    {
        const reorder::DecodedInst &p = scheduledInsts[stalled - back];
        if (reorder::writesReg(p) && (c.rs1 == p.rd || c.rs2 == p.rd))
        {
            producer = stalled - back;
            break;
        }
    }
    if (producer == stalled)
    {
        return StallReason::BlockBoundary; // the producer is before the block
    }
    const reorder::DecodedInst &p = scheduledInsts[producer];
    const bool trueRead = (c.reads_rs1 && c.rs1 == p.rd) || (c.reads_rs2 && c.rs2 == p.rd);
    if (!trueRead)
    {
        return StallReason::FalseStall;
    }

    size_t movableCount = 0;
    for (const reorder::DecodedInst &d : originalInsts)
    {
        movableCount += (d.is_barrier || d.is_pinned) ? 0 : 1;
    }
    if (movableCount <= 2)
    {
        return StallReason::BlockBoundary; // nothing else in the block could ever fill the gap
    }

    const std::vector<bool> busyScheduled = busyFillers(scheduledInsts, kind);
    std::vector<bool> busyOriginal(originalInsts.size(), false);
    size_t busyCount = 0;
    for (size_t k = 0; k < scheduledInsts.size(); ++k)
    {
        if (busyScheduled[k])
        {
            busyOriginal[originalLocalOf[k]] = true;
            ++busyCount;
        }
    }
    // A filler appears only once the memory-only edges are dropped: memory ordering blocks it.
    const size_t pOrig = originalLocalOf[producer];
    const size_t cOrig = originalLocalOf[stalled];
    if (!fillerExists(reachability(graph), originalInsts, busyOriginal, pOrig, cOrig) &&
        fillerExists(reachability(withoutMemoryEdges(graph, originalInsts)), originalInsts,
                     busyOriginal, pOrig, cOrig))
    {
        return StallReason::MemoryOrdering;
    }
    const std::vector<int> cp = reorder::criticalPathLengths(graph);
    const size_t longest = cp.empty() ? 0 : static_cast<size_t>(*std::max_element(cp.begin(), cp.end()));
    if (longest + busyCount >= graph.size)
    {
        return StallReason::DependencyChain; // all that is not already a filler is one chain
    }
    return StallReason::NothingIndependent;
}

// The reason for one remaining stall, given the optimizer's verdict on its block:
// `achievedScore` is the schedule's scoreAfter: the same units as optimum.optimalStalls (a score,
// because the optimizer is called with the scheduler's BlockScoring).
//  - it found a lower score than the schedule      -> SchedulerMissedOptimum
//  - it did not prove the block optimal            -> OtherNotProvenOptimal
//  - proven optimal                                -> a reason from the dependency graph
inline StallReason classifyStall(const reorder::OptimalBlockResult &optimum, int achievedScore,
                                 const std::vector<reorder::DecodedInst> &originalInsts,
                                 const reorder::DepGraph &graph,
                                 const std::vector<reorder::DecodedInst> &scheduledInsts,
                                 const std::vector<size_t> &originalLocalOf, size_t stalled,
                                 PipelineKind kind)
{
    if (optimum.optimalStalls < achievedScore)
    {
        return StallReason::SchedulerMissedOptimum;
    }
    if (!optimum.provenOptimal())
    {
        return StallReason::OtherNotProvenOptimal;
    }
    return graphReason(originalInsts, graph, scheduledInsts, originalLocalOf, stalled, kind);
}

// Every block that has predicted stalls before or after scheduling. `report` must come from
// scheduleProgram(program.text_buffer, kind, labelLeaders(program), ...) with
// ContextMode::BlockEntries and `loopWeight` (either ScheduleMode).
inline std::vector<BlockOutcome> classifyBlocks(const AssembledProgram &program,
                                                const ScheduleReport &report, PipelineKind kind,
                                                int loopWeight = reorder::kDefaultLoopWeight)
{
    std::vector<BlockOutcome> outcomes;
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> leaders = reorder::labelLeaders(program);
    const std::vector<reorder::Block> blocks = reorder::findBlocks(words, leaders);
    const std::vector<reorder::BlockEntry> entries =
        reorder::blockEntries(words, blocks, leaders, loopWeight);
    const std::vector<size_t> original = reorder::originalIndexOf(report);

    for (size_t b = 0; b < report.blocks.size() && b < blocks.size(); ++b)
    {
        const reorder::BlockStalls &bs = report.blocks[b];
        if (bs.scoreBefore == 0 && bs.scoreAfter == 0 && bs.stallsBefore == 0 && bs.stallsAfter == 0)
        {
            continue;
        }
        const reorder::Block &block = bs.block;
        BlockOutcome outcome;
        outcome.block = block;
        outcome.firstLine = reorder::sourceLineOf(program, block.begin);
        outcome.lastLine = reorder::sourceLineOf(program, block.end - 1);
        outcome.stallsBefore = bs.stallsBefore;
        outcome.stallsAfter = bs.stallsAfter;
        outcome.scoreBefore = bs.scoreBefore;
        outcome.scoreAfter = bs.scoreAfter;
        outcome.status = bs.scoreAfter > bs.scoreBefore    ? "worse"
                         : bs.scoreAfter == 0               ? "improved"
                         : bs.scoreAfter < bs.scoreBefore ? "partial"
                                                            : "unimproved";
        if (bs.scoreAfter > 0)
        {
            std::vector<reorder::DecodedInst> originalInsts, scheduledInsts;
            std::vector<size_t> originalLocalOf;
            for (size_t k = block.begin; k < block.end; ++k)
            {
                originalInsts.push_back(reorder::decode(words[k]));
                scheduledInsts.push_back(reorder::decode(report.words[k]));
                originalLocalOf.push_back(original[k] - block.begin);
            }
            const reorder::BlockScoring scoring = schedulerScoring(words, report.words, blocks, entries, b);
            const reorder::OptimalBlockResult optimum =
                reorder::scheduleBlockOptimalDetailed(words, block, kind, scoring);
            outcome.searched = true;
            outcome.optimalScore = optimum.optimalStalls; // a score: see OptimalBlockResult
            outcome.greedyScore = optimum.greedyStalls;
            outcome.method = optimum.method;
            outcome.proven = optimum.provenOptimal();

            // Split the block's remaining score: each stall under each head counts head.weight,
            // and stalls caused in the next block's first instructions count tailWeight.
            const reorder::DepGraph graph = reorder::buildDepGraph(words, block);
            for (const reorder::ScoredHead &head : scoring.heads)
            {
                reorder::WindowState window = head.window;
                for (size_t k = 0; k < scheduledInsts.size(); ++k)
                {
                    const int stalls = reorder::issue(window, scheduledInsts[k], kind, scoring.model);
                    if (stalls > 0)
                    {
                        const StallReason reason =
                            classifyStall(optimum, bs.scoreAfter, originalInsts, graph, scheduledInsts,
                                          originalLocalOf, k, kind);
                        outcome.stallCyclesByReason[reason] += head.weight * stalls;
                    }
                }
                int tailStalls = 0;
                for (const reorder::DecodedInst &d : scoring.tail)
                {
                    tailStalls += reorder::issue(window, d, kind, scoring.model);
                }
                if (tailStalls > 0)
                {
                    const StallReason reason =
                        optimum.optimalStalls < bs.scoreAfter ? StallReason::SchedulerMissedOptimum
                        : !optimum.provenOptimal()             ? StallReason::OtherNotProvenOptimal
                                                               : StallReason::BlockBoundary;
                    outcome.stallCyclesByReason[reason] += scoring.tailWeight * tailStalls;
                }
            }
            // The block is labelled by the reason behind most of its remaining score
            // (ties: the reason listed first in StallReason).
            int bestCycles = 0;
            for (const auto &[reason, cycles] : outcome.stallCyclesByReason)
            {
                if (cycles > bestCycles)
                {
                    bestCycles = cycles;
                    outcome.reason = reason;
                    outcome.hasReason = true;
                }
            }
        }
        outcomes.push_back(outcome);
    }
    return outcomes;
}

// ---------------------------------------------------------------------------------------------
// Batch run
// ---------------------------------------------------------------------------------------------

struct FilterRow
{
    std::string path;
    std::string name;
    std::string category;
    std::string source;
    size_t instructions = 0;
    bool valid = false;
    std::string duplicateOf;
    std::vector<std::string> problems;

    bool usable() const { return valid && duplicateOf.empty(); }
};

struct ResultRow
{
    std::string name;
    std::string category;
    std::string source;
    PipelineKind kind = PipelineKind::HF;
    size_t instructions = 0;
    reorder_report::ReorderOutcome outcome; // raw: ScheduleMode::Optimal, every block reordered
    std::vector<BlockOutcome> blocks;

    // The VM safety net (applyWithVmCheck): block by block, a new order is kept only if the
    // whole program stays equivalent and gets no slower.
    unsigned keptCycles = 0;
    size_t blocksTried = 0;    // blocks whose order changed
    size_t blocksRejected = 0; // ... and that the safety net rejected
    bool originalHitCap = false;

    // Blocks were reordered, but the safety net kept none of them: the original program is kept.
    bool fellBack() const { return blocksTried > 0 && blocksRejected == blocksTried; }
};

struct BenchRun
{
    std::vector<FilterRow> filter;
    std::vector<ResultRow> results;
};

inline std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Every .s file under `root`, reference programs first, then by path (the first of a set of
// near-duplicates is kept).
inline std::vector<std::filesystem::path> findPrograms(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".s")
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end(),
              [](const std::filesystem::path &a, const std::filesystem::path &b)
              {
                  const bool aRef = a.string().find("reference") != std::string::npos;
                  const bool bRef = b.string().find("reference") != std::string::npos;
                  return aRef != bRef ? aRef : a < b;
              });
    return paths;
}

// The final pipeline for one program on one VM: schedule every block with the optimizer
// (ScheduleMode::Optimal, block-entry scoring), run that raw result, then let the VM safety net
// keep only the block orders that keep the program equivalent and no slower.
template <class VM>
void runOptimalWithSafetyNet(const AssembledProgram &program, PipelineKind kind, const Seed &seed,
                             ResultRow &row)
{
    reorder_report::ReorderOutcome &o = row.outcome;
    o.kind = kind;
    o.cycleCap = kBenchCycleCap;
    o.original = program;
    o.report = reorder::scheduleProgram(program.text_buffer, kind, reorder::labelLeaders(program),
                                        true, reorder::ScheduleMode::Optimal);
    o.reordered = withText(program, o.report.words);
    o.before = runProgram<VM>(o.original, seed, kBenchCycleCap, reorder_report::kDataWindowWords);
    o.after = runProgram<VM>(o.reordered, seed, kBenchCycleCap, reorder_report::kDataWindowWords);
    o.equivalent = sameState(o.before, o.after);

    const reorder::SafetyNetResult net =
        reorder::applyWithVmCheck<VM>(program, o.report, seed, kBenchCycleCap);
    row.keptCycles = net.finalCycles;
    row.blocksTried = net.trials.size();
    row.blocksRejected = net.rejectedBlocks().size();
    row.originalHitCap = net.originalHitCap;
}

inline BenchRun runBench(const std::vector<std::filesystem::path> &paths,
                         const std::function<void(const std::string &)> &progress = {})
{
    BenchRun run;
    struct Usable
    {
        size_t filterIndex;
        AssembledProgram program;
        Seed seed;
        std::vector<std::string> shape;
    };
    std::vector<Usable> usable;

    for (const auto &path : paths)
    {
        FilterRow row;
        row.path = path.string();
        const std::string source = readFile(path);
        const ProgramHeader header = parseProgramHeader(source);
        row.name = header.has("name") ? header.get("name") : path.stem().string();
        row.category = header.get("category");
        row.source = header.get("source");
        if (progress)
        {
            progress(row.name);
        }

        const std::string hangLabel = assemblerHangLabel(source);
        if (!hangLabel.empty())
        {
            row.problems.push_back("label named '" + hangLabel +
                                   "' (a section directive): the Kites assembler hangs on it");
            run.filter.push_back(row);
            continue;
        }
        AssembledProgram program;
        try
        {
            program = assemble(path.string());
        }
        catch (const std::exception &e)
        {
            row.problems.push_back(std::string("does not assemble: ") + e.what());
            run.filter.push_back(row);
            continue;
        }
        row.instructions = program.text_buffer.size();
        Seed seed;
        row.problems = staticValidityProblems(program, header, seed);
        if (row.problems.empty())
        {
            const RunResult hf = runProgram<RV5StageProcessorHF>(program, seed, kBenchCycleCap);
            const RunResult hnf = runProgram<RV5StageProcessorHNF>(program, seed, kBenchCycleCap);
            row.problems = dynamicValidityProblems(hf, hnf);
        }
        row.valid = row.problems.empty();
        if (row.valid)
        {
            const std::vector<std::string> shape = mnemonicSequence(program.text_buffer);
            for (const Usable &u : usable)
            {
                if (shapeSimilarity(shape, u.shape) >= kDuplicateThreshold)
                {
                    row.duplicateOf = run.filter[u.filterIndex].name;
                    break;
                }
            }
            if (row.duplicateOf.empty())
            {
                usable.push_back({run.filter.size(), program, seed, shape});
            }
        }
        run.filter.push_back(row);
    }

    for (const Usable &u : usable)
    {
        const FilterRow &f = run.filter[u.filterIndex];
        for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
        {
            ResultRow r;
            r.name = f.name;
            r.category = f.category;
            r.source = f.source;
            r.kind = kind;
            r.instructions = f.instructions;
            if (kind == PipelineKind::HF)
            {
                runOptimalWithSafetyNet<RV5StageProcessorHF>(u.program, kind, u.seed, r);
            }
            else
            {
                runOptimalWithSafetyNet<RV5StageProcessorHNF>(u.program, kind, u.seed, r);
            }
            r.blocks = classifyBlocks(u.program, r.outcome.report, kind);
            run.results.push_back(std::move(r));
        }
    }
    return run;
}

// ---------------------------------------------------------------------------------------------
// CSV and summary
// ---------------------------------------------------------------------------------------------

inline std::string csvField(const std::string &text)
{
    if (text.find_first_of(",\"\n") == std::string::npos)
    {
        return text;
    }
    std::string out = "\"";
    for (char c : text)
    {
        out += (c == '"') ? std::string("\"\"") : std::string(1, c);
    }
    return out + "\"";
}

inline std::string fixed(double value, int digits)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

inline double reductionPercent(unsigned before, unsigned after)
{
    return before == 0 ? 0.0 : 100.0 * (static_cast<double>(before) - after) / before;
}

// What the safety net kept: every reordered block, some of them, or the original program.
inline const char *keptLabel(const ResultRow &r)
{
    if (r.blocksTried == 0 || r.blocksRejected == r.blocksTried)
    {
        return "original";
    }
    return r.blocksRejected == 0 ? "reordered" : "partial";
}

inline std::string filterCsv(const BenchRun &run)
{
    std::ostringstream out;
    out << "program,category,source,instructions,valid,usable,duplicate_of,problems\n";
    for (const FilterRow &f : run.filter)
    {
        std::string problems;
        for (const std::string &p : f.problems)
        {
            problems += (problems.empty() ? "" : "; ") + p;
        }
        out << csvField(f.name) << ',' << csvField(f.category) << ',' << csvField(f.source) << ','
            << f.instructions << ',' << (f.valid ? "yes" : "no") << ','
            << (f.usable() ? "yes" : "no") << ',' << csvField(f.duplicateOf) << ','
            << csvField(problems) << '\n';
    }
    return out.str();
}

inline std::string resultsCsv(const BenchRun &run)
{
    std::ostringstream out;
    out << "program,category,source,vm,instructions,retired_before,retired_after,cycles_before,"
           "cycles_after,cycles_saved,reduction_pct,stalls_before,stalls_after,"
           "predicted_stalls_before,predicted_stalls_after,predicted_score_before,"
           "predicted_score_after,cpi_before,cpi_after,moves,"
           "equivalent,faster,hit_cap,kept,kept_cycles,safety_net_fell_back,blocks_tried,"
           "blocks_rejected\n";
    for (const ResultRow &r : run.results)
    {
        const auto &o = r.outcome;
        out << csvField(r.name) << ',' << r.category << ',' << r.source << ','
            << reorder_report::vmName(r.kind) << ',' << r.instructions << ',' << o.before.retired
            << ',' << o.after.retired << ',' << o.before.cycles << ',' << o.after.cycles << ','
            << static_cast<long long>(o.before.cycles) - static_cast<long long>(o.after.cycles)
            << ',' << fixed(reductionPercent(o.before.cycles, o.after.cycles), 2) << ','
            << o.before.stalls << ',' << o.after.stalls << ',' << o.report.stallsBefore << ','
            << o.report.stallsAfter << ',' << o.report.scoreBefore << ',' << o.report.scoreAfter << ','
            << reorder_report::formatCpi(o.before.cycles, o.before.retired)
            << ',' << reorder_report::formatCpi(o.after.cycles, o.after.retired) << ','
            << o.report.moves.size() << ',' << (o.equivalent ? "yes" : "no") << ','
            << (o.after.cycles < o.before.cycles ? "yes" : "no") << ','
            << (o.before.hitCap || o.after.hitCap ? "yes" : "no") << ','
            << keptLabel(r) << ',' << r.keptCycles << ',' << (r.fellBack() ? "yes" : "no") << ','
            << r.blocksTried << ',' << r.blocksRejected << '\n';
    }
    return out.str();
}

inline std::string blocksCsv(const BenchRun &run)
{
    std::ostringstream out;
    out << "program,category,vm,first_line,last_line,instructions,predicted_stalls_before,"
           "predicted_stalls_after,score_before,score_after,optimal_score,greedy_score,method,"
           "proven,status,reason,"
           "score_by_reason\n";
    for (const ResultRow &r : run.results)
    {
        for (const BlockOutcome &b : r.blocks)
        {
            out << csvField(r.name) << ',' << r.category << ',' << reorder_report::vmName(r.kind)
                << ',' << b.firstLine << ',' << b.lastLine << ',' << (b.block.end - b.block.begin)
                << ',' << b.stallsBefore << ',' << b.stallsAfter << ',' << b.scoreBefore << ','
                << b.scoreAfter << ','
                << (b.searched ? std::to_string(b.optimalScore) : std::string("")) << ','
                << (b.searched ? std::to_string(b.greedyScore) : std::string("")) << ','
                << (b.searched ? csvField(reorder::optimizerMethodName(b.method)) : std::string(""))
                << ',' << (b.searched ? (b.proven ? "yes" : "no") : "") << ',' << b.status << ',' << (b.hasReason ? stallReasonName(b.reason) : "") << ',';
            std::string breakdown;
            for (const auto &[reason, cycles] : b.stallCyclesByReason)
            {
                breakdown += (breakdown.empty() ? "" : " ") + std::string(stallReasonName(reason)) +
                             ":" + std::to_string(cycles);
            }
            out << breakdown << '\n';
        }
    }
    return out.str();
}

inline std::string summaryMarkdown(const BenchRun &run)
{
    std::ostringstream out;
    size_t valid = 0, usableCount = 0, duplicates = 0;
    std::map<std::string, size_t> rejectReasons;
    for (const FilterRow &f : run.filter)
    {
        valid += f.valid ? 1 : 0;
        usableCount += f.usable() ? 1 : 0;
        duplicates += (f.valid && !f.duplicateOf.empty()) ? 1 : 0;
        for (const std::string &p : f.problems)
        {
            rejectReasons[p.substr(0, p.find(':') == std::string::npos ? p.size() : p.find(':'))]++;
        }
    }
    out << "# Reordering benchmark results\n\n";
    out << "## Validity filter\n\n";
    out << "- programs found: " << run.filter.size() << "\n- valid: " << valid
        << "\n- near-duplicates removed: " << duplicates << "\n- usable: " << usableCount << "\n";
    if (!rejectReasons.empty())
    {
        out << "- rejection reasons (a program can have several):\n";
        for (const auto &[reason, count] : rejectReasons)
        {
            out << "  - " << reason << ": " << count << "\n";
        }
    }

    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        size_t tried = 0, improved = 0, unchanged = 0, worse = 0, notEquivalent = 0;
        unsigned long long cyclesBefore = 0, cyclesAfter = 0, cyclesKept = 0, retired = 0;
        size_t fellBack = 0, partiallyKept = 0, keptImproved = 0;
        double sumReduction = 0, sumReductionImproved = 0, maxReduction = 0;
        std::string maxName;
        std::map<std::string, std::pair<size_t, size_t>> byCategory; // tried, improved
        std::map<std::string, std::pair<unsigned long long, unsigned long long>> categoryCycles;
        for (const ResultRow &r : run.results)
        {
            if (r.kind != kind)
            {
                continue;
            }
            const auto &o = r.outcome;
            ++tried;
            notEquivalent += o.equivalent ? 0 : 1;
            const double reduction = reductionPercent(o.before.cycles, o.after.cycles);
            if (o.after.cycles < o.before.cycles)
            {
                ++improved;
                sumReductionImproved += reduction;
                byCategory[r.category].second++;
            }
            else if (o.after.cycles == o.before.cycles)
            {
                ++unchanged;
            }
            else
            {
                ++worse;
            }
            byCategory[r.category].first++;
            categoryCycles[r.category].first += o.before.cycles;
            categoryCycles[r.category].second += o.after.cycles;
            sumReduction += reduction;
            if (reduction > maxReduction)
            {
                maxReduction = reduction;
                maxName = r.name;
            }
            cyclesBefore += o.before.cycles;
            cyclesAfter += o.after.cycles;
            cyclesKept += r.keptCycles;
            fellBack += r.fellBack() ? 1 : 0;
            partiallyKept += std::string(keptLabel(r)) == "partial" ? 1 : 0;
            keptImproved += r.keptCycles < o.before.cycles ? 1 : 0;
            retired += o.before.retired;
        }
        out << "\n## " << reorder_report::vmName(kind) << "\n\n";
        out << "Scheduled with ScheduleMode::Optimal (block-entry scoring). \"total cycles\" is the "
               "raw reordered program; the safety-net line is what is kept.\n\n";
        out << "- programs tried: " << tried << ", improved: " << improved
            << ", unchanged: " << unchanged << ", slower: " << worse
            << ", not equivalent: " << notEquivalent << "\n";
        out << "- total cycles: " << cyclesBefore << " -> " << cyclesAfter << " ("
            << fixed(reductionPercent(static_cast<unsigned>(cyclesBefore),
                                      static_cast<unsigned>(cyclesAfter)), 2)
            << "% fewer)\n";
        out << "- after the VM safety net (applyWithVmCheck keeps a block's new order only if the "
               "program stays equivalent and gets no slower): "
            << cyclesBefore << " -> " << cyclesKept << " ("
            << fixed(reductionPercent(static_cast<unsigned>(cyclesBefore),
                                      static_cast<unsigned>(cyclesKept)), 2)
            << "% fewer); programs improved: " << keptImproved << ", fell back to the original: "
            << fellBack << ", some blocks rejected: " << partiallyKept << "\n";
        out << "- overall CPI: "
            << fixed(retired ? static_cast<double>(cyclesBefore) / retired : 0, 3) << " -> "
            << fixed(retired ? static_cast<double>(cyclesAfter) / retired : 0, 3) << "\n";
        out << "- mean cycle reduction per program: " << fixed(tried ? sumReduction / tried : 0, 2)
            << "% (over all tried), "
            << fixed(improved ? sumReductionImproved / improved : 0, 2)
            << "% (over improved only)\n";
        out << "- max cycle reduction: " << fixed(maxReduction, 2) << "% (" << maxName << ")\n\n";
        out << "| category | tried | improved | cycles before | cycles after | reduction |\n";
        out << "|---|---|---|---|---|---|\n";
        for (const auto &[category, counts] : byCategory)
        {
            const auto &cycles = categoryCycles[category];
            out << "| " << category << " | " << counts.first << " | " << counts.second << " | "
                << cycles.first << " | " << cycles.second << " | "
                << fixed(reductionPercent(static_cast<unsigned>(cycles.first),
                                          static_cast<unsigned>(cycles.second)), 2)
                << "% |\n";
        }

        // Blocks.
        size_t stalledBlocks = 0, improvedBlocks = 0, partialBlocks = 0, unimprovedBlocks = 0,
               worseBlocks = 0;
        std::map<std::string, size_t> unimprovedReasons, remainingReasons;
        for (const ResultRow &r : run.results)
        {
            if (r.kind != kind)
            {
                continue;
            }
            for (const BlockOutcome &b : r.blocks)
            {
                if (b.status == "worse")
                {
                    ++worseBlocks;
                }
                if (b.scoreBefore == 0)
                {
                    continue;
                }
                ++stalledBlocks;
                if (b.status == "worse")
                {
                    ++unimprovedBlocks; // counted with the unchanged ones below as well
                }
                else if (b.status == "improved")
                {
                    ++improvedBlocks;
                }
                else if (b.status == "partial")
                {
                    ++partialBlocks;
                }
                else
                {
                    ++unimprovedBlocks;
                }
                if (b.hasReason)
                {
                    remainingReasons[stallReasonName(b.reason)]++;
                    if (b.status == "unimproved" || b.status == "worse")
                    {
                        unimprovedReasons[stallReasonName(b.reason)]++;
                    }
                }
            }
        }
        out << "\nBlock counts below use the scheduler's weighted block score (scoreBefore/"
               "scoreAfter: stalls per way into the block, loop entries weighted "
            << reorder::kDefaultLoopWeight
            << ", plus stalls caused in the next block's first instructions), the objective the "
               "scheduler and optimizer minimize; plain predicted stalls are in blocks.csv "
               "(predicted_stalls_*). Remaining score is classified only where the optimizer proved "
               "it optimal.\n";
        out << "\nBlocks with a predicted stall score: " << stalledBlocks << " (score removed: "
            << improvedBlocks << ", reduced: " << partialBlocks
            << ", not reduced: " << unimprovedBlocks << "). Blocks with a higher score after"
            << " reordering (a stall moved to their head by the block before): " << worseBlocks
            << "\n\n";
        out << "| reason | blocks not reduced | % | all blocks with stalls left | % |\n";
        out << "|---|---|---|---|---|\n";
        size_t remainingTotal = 0;
        for (const auto &[reason, count] : remainingReasons)
        {
            remainingTotal += count;
        }
        for (StallReason reason :
             {StallReason::DependencyChain, StallReason::NothingIndependent,
              StallReason::MemoryOrdering, StallReason::BlockBoundary, StallReason::FalseStall,
              StallReason::OtherNotProvenOptimal, StallReason::SchedulerMissedOptimum})
        {
            const std::string name = stallReasonName(reason);
            const size_t u = unimprovedReasons[name], all = remainingReasons[name];
            out << "| " << name << " | " << u << " | "
                << fixed(unimprovedBlocks ? 100.0 * u / unimprovedBlocks : 0, 1) << "% | " << all
                << " | " << fixed(remainingTotal ? 100.0 * all / remainingTotal : 0, 1) << "% |\n";
        }
    }
    return out.str();
}
} // namespace Kites::bench_eval
