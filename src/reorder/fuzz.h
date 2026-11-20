#pragma once
// Fuzz harness: runs each program and its reordered version (the reorder-bench pipeline:
// ScheduleMode::Optimal, then the VM safety net) on rv5-h-f and rv5-h-nf from many random
// starting states, and compares the final states with sameState().
//
// Random inputs stay valid by construction (decided statically per program, see analyzeInputs):
//  - registers x1..x31 get random 64-bit values, except registers the program's header declares
//    as inputs (they keep the declared value) and registers that may be read before they are
//    written AND whose value can reach a load/store base or a jalr target (they keep 0), so a
//    random value can never become an address;
//  - every word of the compared data window gets a random value, unless a value loaded from
//    memory can reach an address (e.g. a histogram index or a linked-list next field): then the
//    program's own data keeps its values and only the window words past it are randomized.
// Each input is checked first: if the original program hits the cycle cap or overwrites its own
// instructions with it, the input is reported separately, never as a pass.
//
// Every input is reproducible from (RNG seed, program index, input index); the derived per-input
// seed is printed with any mismatch.
//
// Besides the final-state comparison, every fuzzed program goes through these checks (all
// test tooling; none of them changes what the reorder pipeline produces):
//  - never slower: inputs on which the reordered program takes more cycles than the original
//    are counted and listed (the safety net only promises "not slower" on the declared input);
//  - block invariants (blockInvariantProblems): same block boundaries, every block a
//    permutation of its original words, branch/jump terminators last, every auipc in its slot;
//  - replay exactness (FuzzOptions::checkReplay): path replay's cycles and total_stalls_ equal
//    the VM's, before and after reordering, on the declared input and the first few random ones;
//  - unrolling (FuzzOptions::checkUnroll): a program whose loops get unrolled keeps the same
//    final state on the declared input and every random input.
//
// Two program generators live here: makeRandomStraightLineProgram() (the original 100-program
// set, unchanged) and makeRandomStructuredProgram() (branches, bounded loops, every load/store
// size, la in the middle of blocks, edge-case immediates; see its comment).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "reorder/bench_eval.h"
#include "reorder/loop_unroll.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder/unroll_predict.h"
#include "reorder/vm_safety_net.h"

namespace Kites::fuzz
{
using reorder::PipelineKind;

constexpr uint64_t kDefaultRngSeed = 20261118;
constexpr unsigned kDefaultInputsPerProgram = 20;
constexpr unsigned kDefaultRandomPrograms = 100;

struct FuzzOptions
{
    unsigned inputsPerProgram = kDefaultInputsPerProgram;
    uint64_t rngSeed = kDefaultRngSeed;
    unsigned cycleCap = bench_eval::kBenchCycleCap;
    // Path replay on the declared input and the first replayInputs random inputs.
    bool checkReplay = false;
    unsigned replayInputs = 3;
    // The hazard rule the replay check uses. Always Exact (what the VM does), except in the
    // negative control that must see the replay check fail with the Textbook rule.
    reorder::HazardModel replayModel = reorder::HazardModel::Exact;
    // Unroll + reorder (loop_unroll.h) every program with a source text, and compare the final
    // state of the unrolled program on every input.
    bool checkUnroll = false;
};

struct FuzzProgram
{
    std::string name;
    AssembledProgram program;
    Seed declared;      // the inputs the program's header declares (empty for generated programs)
    std::string source; // the text it was assembled from (needed to unroll); may be empty
};

// ---------------------------------------------------------------------------------------------
// Which inputs may be randomized
// ---------------------------------------------------------------------------------------------

struct InputPlan
{
    std::array<bool, 32> fixedRegister{}; // true: keep the declared value (or 0); x0 is always fixed
    bool memoryFeedsAddress = false;      // a loaded value can reach an address
    size_t dataBytes = 0;                 // the program's own .data size
};

// Registers that may be read before the program writes them. A register written in the entry
// block (which every path starts with) before any read there is defined; everything else may be.
inline std::array<bool, 32> possiblyLiveIn(const AssembledProgram &program)
{
    std::array<bool, 32> liveIn{};
    liveIn.fill(true);
    liveIn[0] = false;
    const std::vector<uint32_t> &words = program.text_buffer;
    if (words.empty())
    {
        return liveIn;
    }
    const std::vector<reorder::Block> blocks = reorder::findBlocks(words, reorder::labelLeaders(program));
    const size_t entryEnd = blocks.empty() ? words.size() : blocks.front().end;
    std::array<bool, 32> read{};
    for (size_t k = 0; k < entryEnd; ++k)
    {
        const reorder::DecodedInst d = reorder::decode(words[k]);
        if (d.reads_rs1)
        {
            read[d.rs1] = true;
        }
        if (d.reads_rs2)
        {
            read[d.rs2] = true;
        }
        if (reorder::writesReg(d) && !read[d.rd])
        {
            liveIn[d.rd] = false; // defined before any use, on every path
        }
    }
    return liveIn;
}

// Flow-insensitive taint: starting from register `source` (or from every loaded value when
// fromMemory), can the value reach a load/store base or a jalr target? Iterated to a fixpoint,
// so loops are covered; writes never untaint (conservative).
inline bool reachesAddress(const std::vector<uint32_t> &words, int source, bool fromMemory)
{
    std::array<bool, 32> tainted{};
    if (source > 0)
    {
        tainted[static_cast<size_t>(source)] = true;
    }
    std::vector<reorder::DecodedInst> insts;
    for (uint32_t w : words)
    {
        insts.push_back(reorder::decode(w));
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const reorder::DecodedInst &d : insts)
        {
            const bool sourceTainted = (d.reads_rs1 && tainted[d.rs1]) || (d.reads_rs2 && tainted[d.rs2]) ||
                                       (fromMemory && d.is_load);
            if (sourceTainted && reorder::writesReg(d) && !tainted[d.rd])
            {
                tainted[d.rd] = true;
                changed = true;
            }
        }
    }
    for (const reorder::DecodedInst &d : insts)
    {
        const bool usesBase = d.is_load || d.is_store || d.kind == reorder::Kind::Jalr;
        if (usesBase && tainted[d.rs1])
        {
            return true;
        }
    }
    return false;
}

inline InputPlan analyzeInputs(const AssembledProgram &program, const Seed &declared)
{
    InputPlan plan;
    plan.fixedRegister[0] = true;
    for (const auto &[reg, value] : declared.regs)
    {
        (void)value;
        plan.fixedRegister[static_cast<size_t>(reg)] = true;
    }
    const std::array<bool, 32> liveIn = possiblyLiveIn(program);
    for (int r = 1; r < 32; ++r)
    {
        if (!plan.fixedRegister[static_cast<size_t>(r)] && liveIn[static_cast<size_t>(r)] &&
            reachesAddress(program.text_buffer, r, false))
        {
            plan.fixedRegister[static_cast<size_t>(r)] = true;
        }
    }
    plan.memoryFeedsAddress = reachesAddress(program.text_buffer, 0, true);
    plan.dataBytes = bench_eval::dataBytes(program);
    return plan;
}

inline uint64_t splitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline uint64_t inputSeedFor(uint64_t rngSeed, size_t programIndex, size_t input)
{
    return splitMix64(splitMix64(rngSeed ^ (static_cast<uint64_t>(programIndex) << 32)) + input);
}

// Mostly small values, so data-driven loop counts stay bounded; sometimes a full 32-bit word.
inline uint32_t randomDataWord(std::mt19937_64 &rng)
{
    const unsigned pick = static_cast<unsigned>(rng() % 10);
    if (pick < 5)
    {
        return static_cast<uint32_t>(rng() % 16);
    }
    if (pick < 8)
    {
        return static_cast<uint32_t>(static_cast<int32_t>(rng() % 272) - 16);
    }
    return static_cast<uint32_t>(rng());
}

// The starting state for one input: random where the plan allows, declared values elsewhere.
inline Seed randomSeed(const FuzzProgram &p, const InputPlan &plan, uint64_t inputSeed)
{
    std::mt19937_64 rng(inputSeed);
    Seed seed;
    std::array<bool, 32> declaredReg{};
    for (const auto &[reg, value] : p.declared.regs)
    {
        seed.regs.push_back({reg, value});
        declaredReg[static_cast<size_t>(reg)] = true;
    }
    for (int r = 1; r < 32; ++r)
    {
        const uint64_t value = rng();
        if (!plan.fixedRegister[static_cast<size_t>(r)] && !declaredReg[static_cast<size_t>(r)])
        {
            seed.regs.push_back({r, value});
        }
    }

    const uint64_t dataStart = vm_config::config.getDataSectionStart();
    const size_t windowWords = reorder_report::kDataWindowWords;
    // Declared memory inputs first; random words written after them win inside the window. With
    // memoryFeedsAddress, the program's own data (declared inputs included) keeps its values.
    for (const auto &[address, value] : p.declared.memWords)
    {
        seed.memWords.push_back({address, value});
    }
    const size_t firstRandomWord = plan.memoryFeedsAddress ? (plan.dataBytes + 3) / 4 : 0;
    for (size_t i = 0; i < windowWords; ++i)
    {
        const uint32_t value = randomDataWord(rng);
        if (i >= firstRandomWord)
        {
            seed.memWords.push_back({dataStart + 4ull * i, value});
        }
    }
    return seed;
}

// ---------------------------------------------------------------------------------------------
// Running and comparing
// ---------------------------------------------------------------------------------------------

struct FuzzMismatch
{
    std::string program;
    PipelineKind kind = PipelineKind::HF;
    uint64_t rngSeed = 0;
    size_t input = 0;
    uint64_t inputSeed = 0;
    std::string detail; // the first differing register or data word
};

struct FuzzPipelineStats
{
    size_t runs = 0;          // inputs tried (one original + one reordered run each)
    size_t passes = 0;        // same final state
    size_t mismatches = 0;
    size_t capHits = 0;       // original or reordered hit the cycle cap: not judged
    size_t invalidInputs = 0; // the input made the original overwrite its own code: not judged

    // The declared input (the one the safety net measured), run once per program.
    size_t declaredRuns = 0;
    size_t declaredMismatches = 0;
    // Never slower: inputs where the reordered program took more cycles than the original.
    size_t slowerDeclared = 0;
    size_t slowerRandom = 0;
    // Block invariants, once per program.
    size_t blockChecks = 0;
    size_t blockViolations = 0;
    // Replay exactness: one check per (input, original or reordered).
    size_t replayChecks = 0;
    size_t replayMismatches = 0;
    size_t replayNotJudged = 0; // the trace did not finish (an input made a loop run too long)
    // Unrolling.
    size_t unrollTried = 0;     // programs with a source text, unrolled + reordered
    size_t unrolledPrograms = 0;
    size_t unrollRuns = 0;      // inputs compared on an unrolled program (declared included)
    size_t unrollMismatches = 0;
    size_t unrollNotJudged = 0; // cycle cap hit on that input
};

// An input on which the reordered program is slower than the original.
struct SlowerRun
{
    std::string program;
    PipelineKind kind = PipelineKind::HF;
    bool declared = false;   // the declared input; otherwise random input `input`
    size_t input = 0;
    uint64_t inputSeed = 0;
    unsigned originalCycles = 0;
    unsigned reorderedCycles = 0;
};

// A replay prediction that differs from the VM.
struct ReplayMismatch
{
    std::string program;
    PipelineKind kind = PipelineKind::HF;
    bool declared = false;
    size_t input = 0;
    uint64_t inputSeed = 0;
    bool reordered = false;  // the reordered program (else the original)
    uint64_t predictedCycles = 0, vmCycles = 0;
    uint64_t predictedStalls = 0, vmStalls = 0;
};

// A violated library invariant, or a different final state outside the random inputs (the
// declared input, or an unrolled program). Any of these is a correctness bug.
struct FuzzProblem
{
    std::string program;
    PipelineKind kind = PipelineKind::HF;
    std::string detail;
};

struct FuzzReport
{
    uint64_t rngSeed = 0;
    unsigned inputsPerProgram = 0;
    size_t programs = 0;
    size_t reorderedPrograms[2] = {0, 0}; // programs whose pipeline output differs from the original
    size_t fixedRegisters = 0;            // live-in address registers kept fixed, summed over programs
    size_t memoryKeptPrograms = 0;        // programs whose own data was not randomized
    FuzzPipelineStats stats[2];           // [0] rv5-h-f, [1] rv5-h-nf
    std::vector<FuzzMismatch> mismatches;
    std::map<std::string, size_t> capHitsByProgram; // both pipelines together
    bool checkedReplay = false;
    bool checkedUnroll = false;
    std::vector<SlowerRun> slower;
    std::vector<ReplayMismatch> replayMismatches;
    std::vector<FuzzProblem> invariantViolations;  // block invariants
    std::vector<FuzzProblem> declaredMismatches;   // declared input ends in a different state
    std::vector<FuzzProblem> unrollMismatches;     // unrolled program ends in a different state
};

inline std::string hex(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

// The first difference sameState() would see, in words.
inline std::string describeDifference(const RunResult &original, const RunResult &reordered)
{
    for (size_t r = 0; r < original.regs.size(); ++r)
    {
        if (original.regs[r] != reordered.regs[r])
        {
            return "x" + std::to_string(r) + ": original " + hex(original.regs[r]) + ", reordered " +
                   hex(reordered.regs[r]);
        }
    }
    const uint64_t dataStart = vm_config::config.getDataSectionStart();
    for (size_t i = 0; i < original.dataWords.size() && i < reordered.dataWords.size(); ++i)
    {
        if (original.dataWords[i] != reordered.dataWords[i])
        {
            return "data word " + hex(dataStart + 4 * i) + ": original " + hex(original.dataWords[i]) +
                   ", reordered " + hex(reordered.dataWords[i]);
        }
    }
    for (size_t i = 0; i < original.stackWords.size() && i < reordered.stackWords.size(); ++i)
    {
        if (original.stackWords[i] != reordered.stackWords[i])
        {
            return "stack word " + std::to_string(i) + ": original " + hex(original.stackWords[i]) +
                   ", reordered " + hex(reordered.stackWords[i]);
        }
    }
    return "no difference found";
}

// The reorder-bench pipeline: optimizer, then the VM safety net (judged with the declared inputs).
template <class VM>
std::vector<uint32_t> pipelineWords(const FuzzProgram &p, PipelineKind kind, unsigned cycleCap)
{
    const reorder::ScheduleReport report =
        reorder::scheduleProgram(p.program.text_buffer, kind, reorder::labelLeaders(p.program), true,
                                 reorder::ScheduleMode::Optimal);
    return reorder::applyWithVmCheck<VM>(p.program, report, p.declared, cycleCap).words;
}

// Optional hook to change the reordered words before fuzzing (the negative controls use it).
using ReorderTransform = std::function<std::vector<uint32_t>(const FuzzProgram &, PipelineKind,
                                                             std::vector<uint32_t>)>;

// ---------------------------------------------------------------------------------------------
// Shared checks
// ---------------------------------------------------------------------------------------------

// What must hold between a program and any reordering of it the library produces: the same
// blocks (findBlocks with the program's label leaders), each block a permutation of its
// original words, a branch/jump that ends a block still last, every auipc in its own slot (its
// result depends on its address), and nothing outside a block moved. Empty when all hold.
inline std::vector<std::string> blockInvariantProblems(const AssembledProgram &program,
                                                       const std::vector<uint32_t> &words)
{
    std::vector<std::string> problems;
    const std::vector<uint32_t> &original = program.text_buffer;
    if (words.size() != original.size())
    {
        problems.push_back("the reordered program has " + std::to_string(words.size()) +
                           " instructions, the original " + std::to_string(original.size()));
        return problems;
    }
    const std::vector<size_t> leaders = reorder::labelLeaders(program);
    const std::vector<reorder::Block> before = reorder::findBlocks(original, leaders);
    const std::vector<reorder::Block> after = reorder::findBlocks(words, leaders);
    bool sameBlocks = before.size() == after.size();
    for (size_t b = 0; sameBlocks && b < before.size(); ++b)
    {
        sameBlocks = before[b].begin == after[b].begin && before[b].end == after[b].end &&
                     before[b].hasTerminator == after[b].hasTerminator;
    }
    if (!sameBlocks)
    {
        problems.push_back("the block boundaries changed");
    }
    std::vector<bool> inBlock(original.size(), false);
    for (const reorder::Block &b : before)
    {
        std::vector<uint32_t> a(original.begin() + static_cast<std::ptrdiff_t>(b.begin),
                                original.begin() + static_cast<std::ptrdiff_t>(b.end));
        std::vector<uint32_t> c(words.begin() + static_cast<std::ptrdiff_t>(b.begin),
                                words.begin() + static_cast<std::ptrdiff_t>(b.end));
        std::sort(a.begin(), a.end());
        std::sort(c.begin(), c.end());
        const std::string where = "block " + std::to_string(b.begin) + "-" + std::to_string(b.end - 1);
        if (a != c)
        {
            problems.push_back(where + " is not a permutation of its original instructions");
        }
        if (b.hasTerminator && words[b.end - 1] != original[b.end - 1])
        {
            problems.push_back(where + ": its branch/jump is no longer last");
        }
        for (size_t k = b.begin; k < b.end; ++k)
        {
            inBlock[k] = true;
        }
    }
    for (size_t k = 0; k < original.size(); ++k)
    {
        if ((original[k] & 0x7F) == 0x17 && words[k] != original[k])
        {
            problems.push_back("the auipc at instruction " + std::to_string(k) + " left its slot");
        }
        if (!inBlock[k] && words[k] != original[k])
        {
            problems.push_back("instruction " + std::to_string(k) + " (outside every block) changed");
        }
    }
    return problems;
}

// Replay exactness for one input: path replay of the original's trace, with the original words
// and with the reordered ones, against the VM's cycles and total_stalls_.
template <class VM>
void checkReplay(const FuzzProgram &p, const std::vector<uint32_t> &words, const Seed &seed,
                 const RunResult &before, const RunResult &after, PipelineKind kind, bool declared,
                 size_t input, uint64_t inputSeed, const FuzzOptions &options, FuzzPipelineStats &stats,
                 FuzzReport &report)
{
    const reorder::ExecutedPath path = reorder::tracePath(p.program, seed, 10ull * options.cycleCap);
    if (!path.finished)
    {
        ++stats.replayNotJudged;
        return;
    }
    const std::vector<reorder::Block> blocks =
        reorder::findBlocks(p.program.text_buffer, reorder::labelLeaders(p.program));
    const std::pair<const std::vector<uint32_t> *, const RunResult *> cases[] = {
        {&p.program.text_buffer, &before}, {&words, &after}};
    for (int c = 0; c < 2; ++c)
    {
        const reorder::ReplayResult r =
            reorder::replayPath(*cases[c].first, blocks, path, kind, options.replayModel);
        const RunResult &vm = *cases[c].second;
        ++stats.replayChecks;
        if (r.predictedCycles != vm.cycles || r.countedStalls() != vm.stalls)
        {
            ++stats.replayMismatches;
            report.replayMismatches.push_back({p.name, kind, declared, input, inputSeed, c == 1, r.predictedCycles,
                                               vm.cycles, r.countedStalls(), vm.stalls});
        }
    }
}

inline void noteSlower(const FuzzProgram &p, PipelineKind kind, bool declared, size_t input, uint64_t inputSeed,
                       const RunResult &before, const RunResult &after, FuzzPipelineStats &stats,
                       FuzzReport &report)
{
    if (before.hitCap || after.hitCap || after.cycles <= before.cycles)
    {
        return;
    }
    ++(declared ? stats.slowerDeclared : stats.slowerRandom);
    report.slower.push_back({p.name, kind, declared, input, inputSeed, before.cycles, after.cycles});
}

template <class VM>
void fuzzOnPipeline(const FuzzProgram &p, size_t programIndex, PipelineKind kind,
                    const InputPlan &plan, const FuzzOptions &options,
                    const ReorderTransform &transform, FuzzReport &report)
{
    std::vector<uint32_t> words = pipelineWords<VM>(p, kind, options.cycleCap);
    if (transform)
    {
        words = transform(p, kind, words);
    }
    const size_t slot = kind == PipelineKind::HF ? 0 : 1;
    report.reorderedPrograms[slot] += words != p.program.text_buffer ? 1 : 0;
    const AssembledProgram reordered = withText(p.program, words);
    FuzzPipelineStats &stats = report.stats[slot];

    // Block invariants, on exactly the words that are fuzzed.
    ++stats.blockChecks;
    for (const std::string &problem : blockInvariantProblems(p.program, words))
    {
        ++stats.blockViolations;
        report.invariantViolations.push_back({p.name, kind, problem});
    }

    // Unroll + reorder once; its program is compared on the declared input and every random one.
    bool haveUnrolled = false;
    AssembledProgram unrolled;
    if (options.checkUnroll && !p.source.empty())
    {
        ++stats.unrollTried;
        const reorder::UnrollOutcome o =
            reorder::unrollAndReorder<VM>(p.source, p.program, p.declared, kind, options.cycleCap);
        if (o.unrolled)
        {
            haveUnrolled = true;
            ++stats.unrolledPrograms;
            unrolled = withText(o.finalProgram, o.finalReport.words);
        }
    }
    auto compareUnrolled = [&](const Seed &seed, const RunResult &before, const std::string &which)
    {
        if (!haveUnrolled)
        {
            return;
        }
        const RunResult run = runProgram<VM>(unrolled, seed, options.cycleCap);
        if (run.hitCap)
        {
            ++stats.unrollNotJudged;
            return;
        }
        ++stats.unrollRuns;
        if (!sameState(before, run) || !run.textIntact)
        {
            ++stats.unrollMismatches;
            report.unrollMismatches.push_back(
                {p.name, kind,
                 which + ": " + (run.textIntact ? describeDifference(before, run) : "overwrote its own code")});
        }
    };

    // The declared input: the one the safety net judged.
    const RunResult declaredBefore = runProgram<VM>(p.program, p.declared, options.cycleCap);
    if (!declaredBefore.hitCap && declaredBefore.textIntact)
    {
        const RunResult declaredAfter = runProgram<VM>(reordered, p.declared, options.cycleCap);
        ++stats.declaredRuns;
        if (!sameState(declaredBefore, declaredAfter) || !declaredAfter.textIntact)
        {
            ++stats.declaredMismatches;
            report.declaredMismatches.push_back(
                {p.name, kind,
                 "declared input: " + (declaredAfter.textIntact ? describeDifference(declaredBefore, declaredAfter)
                                                                : std::string("overwrote its own code"))});
        }
        noteSlower(p, kind, true, 0, 0, declaredBefore, declaredAfter, stats, report);
        if (options.checkReplay)
        {
            checkReplay<VM>(p, words, p.declared, declaredBefore, declaredAfter, kind, true, 0, 0, options, stats,
                            report);
        }
        compareUnrolled(p.declared, declaredBefore, "declared input");
    }

    for (size_t input = 0; input < options.inputsPerProgram; ++input)
    {
        const uint64_t inputSeed = inputSeedFor(options.rngSeed, programIndex, input);
        const Seed seed = randomSeed(p, plan, inputSeed);
        ++stats.runs;
        const RunResult before = runProgram<VM>(p.program, seed, options.cycleCap);
        if (before.hitCap)
        {
            ++stats.capHits;
            ++report.capHitsByProgram[p.name];
            continue;
        }
        if (!before.textIntact)
        {
            ++stats.invalidInputs;
            continue;
        }
        compareUnrolled(seed, before, "random input " + std::to_string(input) + " (input seed " +
                                          std::to_string(inputSeed) + ")");
        const RunResult after = runProgram<VM>(reordered, seed, options.cycleCap);
        if (after.hitCap)
        {
            ++stats.capHits;
            ++report.capHitsByProgram[p.name];
            continue;
        }
        noteSlower(p, kind, false, input, inputSeed, before, after, stats, report);
        if (options.checkReplay && input < options.replayInputs)
        {
            checkReplay<VM>(p, words, seed, before, after, kind, false, input, inputSeed, options, stats, report);
        }
        if (sameState(before, after) && after.textIntact)
        {
            ++stats.passes;
            continue;
        }
        ++stats.mismatches;
        report.mismatches.push_back({p.name, kind, options.rngSeed, input, inputSeed,
                                     after.textIntact ? describeDifference(before, after)
                                                      : "reordered program overwrote its own code"});
    }
}

inline FuzzReport fuzzPrograms(const std::vector<FuzzProgram> &programs, const FuzzOptions &options,
                               const ReorderTransform &transform = {},
                               const std::function<void(const std::string &)> &progress = {})
{
    FuzzReport report;
    report.rngSeed = options.rngSeed;
    report.inputsPerProgram = options.inputsPerProgram;
    report.programs = programs.size();
    report.checkedReplay = options.checkReplay;
    report.checkedUnroll = options.checkUnroll;
    for (size_t i = 0; i < programs.size(); ++i)
    {
        const FuzzProgram &p = programs[i];
        if (progress)
        {
            progress(p.name);
        }
        const InputPlan plan = analyzeInputs(p.program, p.declared);
        for (int r = 1; r < 32; ++r)
        {
            bool declared = false;
            for (const auto &[reg, value] : p.declared.regs)
            {
                (void)value;
                declared = declared || reg == r;
            }
            report.fixedRegisters += (plan.fixedRegister[static_cast<size_t>(r)] && !declared) ? 1 : 0;
        }
        report.memoryKeptPrograms += plan.memoryFeedsAddress ? 1 : 0;
        fuzzOnPipeline<RV5StageProcessorHF>(p, i, PipelineKind::HF, plan, options, transform, report);
        fuzzOnPipeline<RV5StageProcessorHNF>(p, i, PipelineKind::HNF, plan, options, transform, report);
    }
    return report;
}

// ---------------------------------------------------------------------------------------------
// Programs to fuzz
// ---------------------------------------------------------------------------------------------

// Every usable benchmark program (the reorder-bench validity filter, near-duplicates removed).
inline std::vector<FuzzProgram> benchmarkPrograms(const std::filesystem::path &root)
{
    std::vector<bench_eval::FilterRow> filter;
    std::vector<FuzzProgram> programs;
    for (const bench_eval::UsableProgram &u : bench_eval::filterPrograms(bench_eval::findPrograms(root), filter))
    {
        programs.push_back({filter[u.filterIndex].name, u.program, u.seed,
                            bench_eval::readFile(filter[u.filterIndex].path)});
    }
    return programs;
}

// Port of makeRandomProgram() in test/reorder_scheduler_test.cpp (same instruction mix, register
// pools and offsets), which lives in that test's anonymous namespace. One change: its three base
// registers point into the compared data window (data start, +64, +128) instead of addresses
// 1000/1200/1400, so every store it makes is checked by sameState().
inline std::string makeRandomStraightLineProgram(std::mt19937 &rng, int bodyLen)
{
    static const int basesReg[] = {10, 11, 12};
    static const int baseOffset[] = {0, 64, 128};
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

    const uint64_t dataStart = vm_config::config.getDataSectionStart();
    std::ostringstream out;
    for (int i : {0, 1, 2})
    {
        out << "lui x" << basesReg[i] << ", " << (dataStart >> 12) << "\n";
        if (baseOffset[i] != 0)
        {
            out << "addi x" << basesReg[i] << ", x" << basesReg[i] << ", " << baseOffset[i] << "\n";
        }
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pickOp(rng);
        if (op == 0)
        {
            const int rd = pool[pickPool(rng)];
            const int rs1 = pool[pickPool(rng)];
            int imm = randImm(rng);
            if (coinFlip(rng))
            {
                const int collideWith = pool[pickPool(rng)];
                imm = (imm & ~0x1F) | collideWith;
            }
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else if (op == 12)
        {
            out << "lw x" << pool[pickPool(rng)] << ", " << offsets[pickOffset(rng)] << "(x"
                << basesReg[pickBase(rng)] << ")\n";
        }
        else if (op == 13)
        {
            out << "sw x" << pool[pickPool(rng)] << ", " << offsets[pickOffset(rng)] << "(x"
                << basesReg[pickBase(rng)] << ")\n";
        }
        else
        {
            out << rtypeOps[op - 1] << " x" << pool[pickPool(rng)] << ", x" << pool[pickPool(rng)]
                << ", x" << pool[pickPool(rng)] << "\n";
        }
    }
    return out.str();
}

// `count` generated programs (lengths 10..20, as in the scheduler test), from `rngSeed`.
inline std::vector<FuzzProgram> randomPrograms(size_t count, uint64_t rngSeed)
{
    std::mt19937 rng(static_cast<std::mt19937::result_type>(splitMix64(rngSeed)));
    std::uniform_int_distribution<int> lenDist(10, 20);
    std::vector<FuzzProgram> programs;
    for (size_t i = 0; i < count; ++i)
    {
        const std::string source = makeRandomStraightLineProgram(rng, lenDist(rng));
        std::istringstream stream(source);
        programs.push_back({"random#" + std::to_string(i), assemble(stream), {}, source});
    }
    return programs;
}

// ---------------------------------------------------------------------------------------------
// Second generator: structured programs
// ---------------------------------------------------------------------------------------------

// Registers makeRandomStructuredProgram() reserves.
constexpr int kStructuredCounterReg = 28;                 // loop counter: only `li` and the decrement write it
constexpr int kStructuredBaseRegs[] = {10, 11, 12, 13};   // always hold an address inside the data window
constexpr const char *kStructuredDataLabels[] = {"b0", "b1", "b2", "b3"}; // 64 bytes each: the whole window

namespace structured_detail
{
// Registers the body may read and write: everything except x0, sp (x2), the bases, a7 (x17,
// set for the exit) and the loop counter.
constexpr int kPool[] = {1, 3, 4, 5, 6, 7, 8, 9, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30, 31};
constexpr int kEdgeImmediates[] = {0, 1, -1, 2047, -2048};
constexpr int kEdgeShifts[] = {0, 1, 31, 32, 63};

class Writer
{
public:
    explicit Writer(std::mt19937_64 &rng) : rng_(rng) {}

    std::string text() const { return out_.str(); }
    int emitted() const { return emitted_; }

    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng_); }
    bool chance(int percent) { return pick(0, 99) < percent; }
    int poolReg() { return kPool[pick(0, static_cast<int>(std::size(kPool)) - 1)]; }
    std::string label(const char *stem) { return std::string(stem) + std::to_string(labels_++); }

    void line(const std::string &text, int words = 1)
    {
        out_ << text << "\n";
        emitted_ += words;
    }

    // One instruction with no control flow; `allowLa` lets it be an la (two words, an auipc).
    void simple(bool allowLa)
    {
        static const char *rtype[] = {"add", "sub", "and", "or", "xor", "sll", "srl", "sra", "slt", "sltu"};
        static const char *mtype[] = {"mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu"};
        // The W instructions the Kites assembler accepts (addw/subw/sllw/srlw/sraw/addiw/slliw/
        // srliw/sraiw are declared but rejected by its parser).
        static const char *wtype[] = {"mulw", "divw", "divuw", "remw", "remuw"};
        static const char *itype[] = {"addi", "xori", "ori", "andi", "slti", "sltiu"};
        static const char *shifts[] = {"slli", "srli", "srai"};
        static const char *loads[] = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld"};
        static const int loadSize[] = {1, 1, 2, 2, 4, 4, 8};
        static const char *stores[] = {"sb", "sh", "sw", "sd"};
        static const int storeSize[] = {1, 2, 4, 8};

        // An immediate whose bits 20-24 name the register a load just wrote (the hazard unit
        // compares them as rs2): about a third of the time while one is pending.
        if (pendingLoadRd_ != 0 && chance(35))
        {
            const int imm = (pick(0, 63) << 5) | pendingLoadRd_;
            reg3(itype[pick(0, 5)], poolReg(), poolReg(), std::to_string(imm));
            return;
        }
        const int k = pick(0, 99);
        if (k < 14)
        {
            reg3(rtype[pick(0, 9)], poolReg(), poolReg(), "x" + std::to_string(poolReg()));
        }
        else if (k < 24)
        {
            reg3(mtype[pick(0, 7)], poolReg(), poolReg(), "x" + std::to_string(poolReg()));
        }
        else if (k < 32)
        {
            reg3(wtype[pick(0, 4)], poolReg(), poolReg(), "x" + std::to_string(poolReg()));
        }
        else if (k < 46)
        {
            const int imm = chance(45) ? kEdgeImmediates[pick(0, 4)] : pick(-2048, 2047);
            reg3(itype[pick(0, 5)], poolReg(), poolReg(), std::to_string(imm));
        }
        else if (k < 54)
        {
            const int sh = chance(55) ? kEdgeShifts[pick(0, 4)] : pick(0, 63);
            reg3(shifts[pick(0, 2)], poolReg(), poolReg(), std::to_string(sh));
        }
        else if (k < 56)
        {
            const int rd = poolReg();
            line("lui x" + std::to_string(rd) + ", " + std::to_string(pick(0, 1048575)));
            written(rd);
        }
        else if (k < 76)
        {
            const int which = pick(0, 6);
            const int rd = poolReg();
            line(std::string(loads[which]) + " x" + std::to_string(rd) + ", " + offset(loadSize[which]) + "(x" +
                 std::to_string(kStructuredBaseRegs[pick(0, 3)]) + ")");
            written(rd);
            pendingLoadRd_ = rd;
            pendingLeft_ = 2;
            return;
        }
        else if (k < 94 || !allowLa)
        {
            const int which = pick(0, 3);
            line(std::string(stores[which]) + " x" + std::to_string(poolReg()) + ", " + offset(storeSize[which]) +
                 "(x" + std::to_string(kStructuredBaseRegs[pick(0, 3)]) + ")");
        }
        else
        {
            // Re-point a base at one of the four data labels: still inside the window.
            line("la x" + std::to_string(kStructuredBaseRegs[pick(0, 3)]) + ", " + kStructuredDataLabels[pick(0, 3)],
                 2);
        }
        age();
    }

    void simples(int n, bool allowLa)
    {
        for (int i = 0; i < n; ++i)
        {
            simple(allowLa);
        }
    }

    // A forward conditional branch (any of the six) over `skip` instructions.
    void forwardBranch(int skip, bool allowLa)
    {
        static const char *branches[] = {"beq", "bne", "blt", "bge", "bltu", "bgeu"};
        const std::string target = label("fwd"); // not "f<n>": that is a register name
        const int rs2 = chance(25) ? 0 : poolReg();
        line(std::string(branches[pick(0, 5)]) + " x" + std::to_string(poolReg()) + ", x" + std::to_string(rs2) +
             ", " + target);
        simples(skip, allowLa);
        line(target + ":", 0);
        resetPending();
    }

    // jal x0 over `skip` instructions (they never run).
    void forwardJump(int skip, bool allowLa)
    {
        const std::string target = label("skip");
        line("jal x0, " + target);
        simples(skip, allowLa);
        line(target + ":", 0);
        resetPending();
    }

    // A countdown loop on the counter register, 2-4 iterations. Without an inner branch it is
    // one basic block ending in a branch to its own first instruction (unrolling applies).
    void loop(bool innerBranch)
    {
        const std::string head = label("loop");
        line("li x" + std::to_string(kStructuredCounterReg) + ", " + std::to_string(pick(2, 4)));
        line(head + ":", 0);
        resetPending();
        if (innerBranch)
        {
            simples(pick(1, 4), false);
            forwardBranch(pick(1, 4), false);
            simples(pick(0, 3), false);
        }
        else
        {
            simples(pick(2, 10), false);
        }
        const std::string counter = "x" + std::to_string(kStructuredCounterReg);
        line("addi " + counter + ", " + counter + ", -1");
        line("bne " + counter + ", x0, " + head);
        resetPending();
    }

private:
    void reg3(const char *op, int rd, int rs1, const std::string &last)
    {
        line(std::string(op) + " x" + std::to_string(rd) + ", x" + std::to_string(rs1) + ", " + last);
        written(rd);
        age();
    }
    void written(int rd)
    {
        if (rd == pendingLoadRd_)
        {
            pendingLoadRd_ = 0;
        }
    }
    void age()
    {
        if (pendingLoadRd_ != 0 && --pendingLeft_ <= 0)
        {
            pendingLoadRd_ = 0;
        }
    }
    void resetPending() { pendingLoadRd_ = 0; }

    // Size-aligned, small (so accesses through the same base overlap), inside the base's 64 bytes.
    std::string offset(int size)
    {
        const int positions = std::min(24, 64 - size) / size; // not "slots": a Qt macro
        return std::to_string(size * pick(0, positions));
    }

    std::mt19937_64 &rng_;
    std::ostringstream out_;
    int emitted_ = 0;
    int labels_ = 0;
    int pendingLoadRd_ = 0;
    int pendingLeft_ = 0;
};
} // namespace structured_detail

// A random program, deterministic in `rng`, that always terminates:
//  - data: four labels b0..b3 of 8 dwords each, exactly the compared 64-word window;
//  - x10..x13 are set with la in the first block (x13 = b0 + 32, so accesses through x10 and
//    x13 overlap) and only ever re-pointed at one of the labels, by la in the middle of a block
//    (an auipc the scheduler must keep in its slot); every load/store size (lb lbu lh lhu lw
//    lwu ld, sb sh sw sd) at a size-aligned offset under 64, so every access stays in the window;
//  - forward conditional branches (all six) and jal x0, each skipping 1-6 instructions, never
//    backward;
//  - countdown loops on x28 (2-4 iterations; only li and the decrement write it): some are a
//    single block (unrolling applies: no la, call, j, ret or ecall inside), some have a forward
//    branch inside;
//  - R-type and M (mul/mulh/mulhsu/mulhu/div/divu/rem/remu) and the W instructions the Kites
//    assembler accepts (mulw divw divuw remw remuw);
//  - edge-case immediates (0, 1, -1, 2047, -2048) and shift amounts (0, 1, 31, 32, 63);
//  - immediates whose bits 20-24 equal the register a load one or two instructions before
//    wrote (the hazard unit's immediate-bits quirk);
//  - 20-80 body instructions, with long straight runs so some blocks reach 30-64 instructions;
//  - ends with li a7, 10 and the exit ecall as the last instruction (the bench validity rule).
inline std::string makeRandomStructuredProgram(std::mt19937_64 &rng)
{
    structured_detail::Writer w(rng);
    std::ostringstream data;
    data << ".data\n";
    for (const char *label : kStructuredDataLabels)
    {
        data << label << ": .dword";
        for (int i = 0; i < 8; ++i)
        {
            data << (i == 0 ? " " : ", ") << w.pick(0, 1000);
        }
        data << "\n";
    }
    w.line(".text", 0);
    for (int i = 0; i < 4; ++i)
    {
        w.line("la x" + std::to_string(kStructuredBaseRegs[i]) + ", " + kStructuredDataLabels[i == 3 ? 0 : i], 2);
    }
    w.line("addi x13, x13, 32");
    w.line("li x" + std::to_string(kStructuredCounterReg) + ", 0");

    const int target = w.pick(20, 80);
    const int start = w.emitted();
    while (w.emitted() - start < target)
    {
        const int left = target - (w.emitted() - start);
        const int k = w.pick(0, 99);
        if (k < 30)
        {
            // A long straight run (the large blocks), sometimes with an la in it.
            w.simples(std::min(left, w.pick(20, 60)), true);
        }
        else if (k < 50)
        {
            w.simples(std::min(left, w.pick(1, 8)), true);
        }
        else if (k < 70)
        {
            w.forwardBranch(w.pick(1, 6), true);
        }
        else if (k < 78)
        {
            w.forwardJump(w.pick(1, 6), true);
        }
        else if (k < 90)
        {
            w.loop(false);
        }
        else
        {
            w.loop(true);
        }
    }
    w.line("li x17, 10");
    w.line("ecall");
    return data.str() + w.text();
}

// `count` structured programs from `rngSeed`: program i is makeRandomStructuredProgram() with an
// RNG seeded from (rngSeed, i), so any one program can be rebuilt on its own. The counter
// register is declared as an input (value 0), so random inputs never touch it.
inline FuzzProgram structuredProgram(uint64_t rngSeed, size_t index)
{
    std::mt19937_64 rng(splitMix64(splitMix64(rngSeed ^ 0x5354525543547552ull) + index));
    const std::string source = makeRandomStructuredProgram(rng);
    std::istringstream stream(source);
    Seed declared;
    declared.regs.push_back({kStructuredCounterReg, 0});
    return {"structured#" + std::to_string(index), assemble(stream), declared, source};
}

inline std::vector<FuzzProgram> structuredPrograms(size_t count, uint64_t rngSeed)
{
    std::vector<FuzzProgram> programs;
    for (size_t i = 0; i < count; ++i)
    {
        programs.push_back(structuredProgram(rngSeed, i));
    }
    return programs;
}

// What a generated program really contains, read from its assembled words (not from the
// generator's bookkeeping), so the mix can be checked.
struct ProgramMix
{
    std::array<size_t, 6> forwardBranches{};  // beq bne blt bge bltu bgeu
    size_t forwardJals = 0;
    size_t backwardBranches = 0;              // loops
    size_t eligibleLoops = 0;                 // single-block loops (unroll_predict.h)
    size_t loopsWithBranch = 0;               // loops with a forward branch inside
    size_t auipcs = 0;
    size_t auipcMidBlock = 0;                 // auipc not at the start of its block
    std::array<size_t, 7> loads{};            // lb lh lw ld lbu lhu lwu (funct3 order)
    std::array<size_t, 4> stores{};           // sb sh sw sd
    size_t mulDiv = 0;                        // opcode 0x33, M extension
    size_t wMulDiv = 0;                       // opcode 0x3B, M extension
    std::array<size_t, 5> edgeImmediates{};   // 0, 1, -1, 2047, -2048 in ALU I-type instructions
    std::array<size_t, 5> edgeShifts{};       // 0, 1, 31, 32, 63
    size_t quirkImmediates = 0;               // I-type bits 20-24 == rd of a load 1-2 instructions before
    size_t largestBlock = 0;
    size_t blocksOf30 = 0;                    // blocks of 30 to 64 instructions
};

inline ProgramMix programMix(const AssembledProgram &program)
{
    ProgramMix m;
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<reorder::Block> blocks = reorder::findBlocks(words, reorder::labelLeaders(program));
    std::vector<bool> blockStart(words.size(), false);
    for (const reorder::Block &b : blocks)
    {
        blockStart[b.begin] = true;
        const size_t n = b.end - b.begin;
        m.largestBlock = std::max(m.largestBlock, n);
        m.blocksOf30 += (n >= 30 && n <= reorder::kMaxOptimalBlockSize) ? 1 : 0;
    }
    m.eligibleLoops = reorder::eligibleLoops(words, blocks).size();
    static const int branchSlot[8] = {0, 1, -1, -1, 2, 3, 4, 5};
    for (size_t k = 0; k < words.size(); ++k)
    {
        const uint32_t w = words[k];
        const uint32_t opcode = w & 0x7F;
        const uint32_t funct3 = (w >> 12) & 0x7;
        const uint32_t funct7 = w >> 25;
        const int32_t imm = static_cast<int32_t>(w) >> 20;
        if (opcode == 0x63)
        {
            const int32_t offset = reorder::branchImm(w);
            if (offset > 0 && branchSlot[funct3] >= 0)
            {
                ++m.forwardBranches[static_cast<size_t>(branchSlot[funct3])];
            }
            else if (offset < 0)
            {
                ++m.backwardBranches;
                const int64_t head = static_cast<int64_t>(k) + offset / 4;
                for (int64_t i = std::max<int64_t>(head, 0); i < static_cast<int64_t>(k); ++i)
                {
                    const uint32_t inner = words[static_cast<size_t>(i)];
                    if ((inner & 0x7F) == 0x63 && reorder::branchImm(inner) > 0)
                    {
                        ++m.loopsWithBranch;
                        break;
                    }
                }
            }
        }
        else if (opcode == 0x6F && reorder::jalImm(w) > 0)
        {
            ++m.forwardJals;
        }
        else if (opcode == 0x17)
        {
            ++m.auipcs;
            m.auipcMidBlock += blockStart[k] ? 0 : 1;
        }
        else if (opcode == 0x03 && funct3 < 7)
        {
            ++m.loads[funct3];
        }
        else if (opcode == 0x23 && funct3 < 4)
        {
            ++m.stores[funct3];
        }
        else if (opcode == 0x33 && funct7 == 1)
        {
            ++m.mulDiv;
        }
        else if (opcode == 0x3B && funct7 == 1)
        {
            ++m.wMulDiv;
        }
        else if (opcode == 0x13 && (funct3 == 1 || funct3 == 5))
        {
            const uint32_t shamt = (w >> 20) & 0x3F;
            for (size_t e = 0; e < std::size(structured_detail::kEdgeShifts); ++e)
            {
                m.edgeShifts[e] += shamt == static_cast<uint32_t>(structured_detail::kEdgeShifts[e]) ? 1 : 0;
            }
        }
        else if (opcode == 0x13)
        {
            for (size_t e = 0; e < std::size(structured_detail::kEdgeImmediates); ++e)
            {
                m.edgeImmediates[e] += imm == structured_detail::kEdgeImmediates[e] ? 1 : 0;
            }
        }
        if (opcode == 0x13 || opcode == 0x1B || opcode == 0x03)
        {
            const uint32_t field = (w >> 20) & 0x1F;
            for (size_t back = 1; back <= 2 && back <= k; ++back)
            {
                const uint32_t prev = words[k - back];
                if ((prev & 0x7F) == 0x03 && ((prev >> 7) & 0x1F) == field && field != 0)
                {
                    ++m.quirkImmediates;
                    break;
                }
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// Optimizer behaviour by block size (reorder-fuzz --optimizer-table)
// ---------------------------------------------------------------------------------------------

struct OptimizerSizeRow
{
    size_t size = 0;
    PipelineKind kind = PipelineKind::HF;
    size_t blocks = 0;
    double meanMs = 0, maxMs = 0;
    size_t proven = 0;            // Optimal or GreedyAlreadyZero
    size_t bestFound = 0;         // node cap hit, better than greedy
    size_t greedyCapHit = 0;      // node cap hit, greedy kept
    long long greedyStalls = 0;   // summed predicted stalls
    long long optimalStalls = 0;
    long long originalStalls = 0;
};

// `blocksPerSize` random straight-line blocks of each size (the structured generator's
// instruction mix, no control flow), scheduled with scheduleBlockOptimalDetailed() from an
// empty window with the default 200,000-position budget, on both pipelines.
inline std::vector<OptimizerSizeRow> optimizerTable(const std::vector<size_t> &sizes, size_t blocksPerSize,
                                                    uint64_t rngSeed)
{
    std::vector<OptimizerSizeRow> rows;
    for (size_t size : sizes)
    {
        OptimizerSizeRow row[2];
        for (size_t b = 0; b < blocksPerSize; ++b)
        {
            std::mt19937_64 rng(splitMix64(splitMix64(rngSeed + size) + b));
            structured_detail::Writer w(rng);
            w.line(".text", 0);
            while (static_cast<size_t>(w.emitted()) < size)
            {
                w.simple(false);
            }
            std::istringstream stream(w.text());
            const AssembledProgram program = assemble(stream);
            const reorder::Block block{0, program.text_buffer.size(), false};
            for (int slot = 0; slot < 2; ++slot)
            {
                const PipelineKind kind = slot == 0 ? PipelineKind::HF : PipelineKind::HNF;
                const auto t0 = std::chrono::steady_clock::now();
                const reorder::OptimalBlockResult r =
                    reorder::scheduleBlockOptimalDetailed(program.text_buffer, block, kind);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                OptimizerSizeRow &o = row[slot];
                o.size = size;
                o.kind = kind;
                ++o.blocks;
                o.meanMs += ms;
                o.maxMs = std::max(o.maxMs, ms);
                o.proven += r.provenOptimal() ? 1 : 0;
                o.bestFound += r.method == reorder::OptimizerMethod::BestFound ? 1 : 0;
                o.greedyCapHit += r.method == reorder::OptimizerMethod::GreedyCapHit ? 1 : 0;
                o.greedyStalls += r.greedyStalls;
                o.optimalStalls += r.optimalStalls;
                o.originalStalls += r.originalStalls;
            }
        }
        for (OptimizerSizeRow &o : row)
        {
            o.meanMs = o.blocks ? o.meanMs / static_cast<double>(o.blocks) : 0;
            rows.push_back(o);
        }
    }
    return rows;
}

inline std::string optimizerTableMarkdown(const std::vector<OptimizerSizeRow> &rows, size_t blocksPerSize,
                                          uint64_t rngSeed)
{
    std::ostringstream out;
    out << "Optimizer by block size: " << blocksPerSize << " random straight-line blocks per size, RNG seed "
        << rngSeed << ", budget " << reorder::kDefaultOptimalNodeBudget << " positions, empty head window\n\n";
    out << "| Block size | Pipeline | Mean ms | Max ms | Proven optimal | Best found (cap) | Greedy kept (cap) "
           "| Original stalls | Greedy stalls | Optimal stalls |\n";
    out << "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    out << std::fixed;
    for (const OptimizerSizeRow &r : rows)
    {
        const double pct = r.blocks ? 100.0 * static_cast<double>(r.proven) / static_cast<double>(r.blocks) : 0;
        out << "| " << r.size << " | " << reorder_report::vmName(r.kind) << " | " << std::setprecision(2)
            << r.meanMs << " | " << r.maxMs << " | " << std::setprecision(0) << pct << "% (" << r.proven << "/"
            << r.blocks << ") | " << r.bestFound << " | " << r.greedyCapHit << " | " << r.originalStalls << " | "
            << r.greedyStalls << " | " << r.optimalStalls << " |\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------------------------

inline std::string summaryText(const FuzzReport &r)
{
    std::ostringstream out;
    out << "Fuzz: " << r.programs << " program(s), " << r.inputsPerProgram
        << " random input(s) each, RNG seed " << r.rngSeed << "\n";
    out << "  live-in address registers kept fixed: " << r.fixedRegisters
        << "; programs whose own data was kept (loaded values reach addresses): "
        << r.memoryKeptPrograms << "\n";
    for (int slot = 0; slot < 2; ++slot)
    {
        const FuzzPipelineStats &s = r.stats[slot];
        out << "  " << (slot == 0 ? "rv5-h-f " : "rv5-h-nf") << ": " << s.runs << " runs ("
            << r.reorderedPrograms[slot] << " program(s) reordered), " << s.passes
            << " same state, " << s.mismatches << " mismatch(es), " << s.capHits
            << " cycle-cap hit(s), " << s.invalidInputs << " invalid input(s)\n";
    }
    if (!r.capHitsByProgram.empty())
    {
        out << "  cycle-cap hits by program (both pipelines; a random input made a data-driven loop run "
               "long):";
        for (const auto &[name, count] : r.capHitsByProgram)
        {
            out << " " << name << " x" << count;
        }
        out << "\n";
    }
    for (const FuzzMismatch &m : r.mismatches)
    {
        out << "  MISMATCH " << m.program << " on " << reorder_report::vmName(m.kind)
            << ": RNG seed " << m.rngSeed << ", input " << m.input << " (input seed " << m.inputSeed
            << "): " << m.detail << "\n";
    }

    // The extra checks.
    for (int slot = 0; slot < 2; ++slot)
    {
        const FuzzPipelineStats &s = r.stats[slot];
        out << "  " << (slot == 0 ? "rv5-h-f " : "rv5-h-nf") << " checks: declared input " << s.declaredRuns
            << " run(s), " << s.declaredMismatches << " mismatch(es); slower than the original: "
            << s.slowerDeclared << " declared, " << s.slowerRandom << " random; block invariants "
            << s.blockChecks << " program(s), " << s.blockViolations << " violation(s)\n";
        if (r.checkedReplay)
        {
            out << "           replay: " << s.replayChecks << " check(s), " << s.replayMismatches
                << " mismatch(es), " << s.replayNotJudged << " input(s) not traced to the end\n";
        }
        if (r.checkedUnroll)
        {
            out << "           unroll: " << s.unrollTried << " program(s) tried, " << s.unrolledPrograms
                << " unrolled, " << s.unrollRuns << " input(s) compared, " << s.unrollMismatches
                << " mismatch(es), " << s.unrollNotJudged << " not judged (cycle cap)\n";
        }
    }
    for (const SlowerRun &x : r.slower)
    {
        out << "  SLOWER " << x.program << " on " << reorder_report::vmName(x.kind) << ": ";
        if (x.declared)
        {
            out << "declared input";
        }
        else
        {
            out << "RNG seed " << r.rngSeed << ", input " << x.input << " (input seed " << x.inputSeed << ")";
        }
        out << ": " << x.originalCycles << " -> " << x.reorderedCycles << " cycles\n";
    }
    for (const ReplayMismatch &x : r.replayMismatches)
    {
        out << "  REPLAY MISMATCH " << x.program << " on " << reorder_report::vmName(x.kind) << ", "
            << (x.reordered ? "reordered" : "original") << ", ";
        if (x.declared)
        {
            out << "declared input";
        }
        else
        {
            out << "input " << x.input << " (input seed " << x.inputSeed << ")";
        }
        out << ": predicted " << x.predictedCycles << " cycles / " << x.predictedStalls << " stalls, VM "
            << x.vmCycles << " / " << x.vmStalls << "\n";
    }
    const std::pair<const char *, const std::vector<FuzzProblem> *> problems[] = {
        {"INVARIANT", &r.invariantViolations},
        {"DECLARED MISMATCH", &r.declaredMismatches},
        {"UNROLL MISMATCH", &r.unrollMismatches}};
    for (const auto &[tag, list] : problems)
    {
        for (const FuzzProblem &x : *list)
        {
            out << "  " << tag << " " << x.program << " on " << reorder_report::vmName(x.kind) << ": " << x.detail
                << "\n";
        }
    }
    return out.str();
}

// True when nothing a correctness check looks at went wrong (slower runs are findings, not
// failures).
inline bool fuzzClean(const FuzzReport &r)
{
    return r.mismatches.empty() && r.replayMismatches.empty() && r.invariantViolations.empty() &&
           r.declaredMismatches.empty() && r.unrollMismatches.empty();
}
} // namespace Kites::fuzz
