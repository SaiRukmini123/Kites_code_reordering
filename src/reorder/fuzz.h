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

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "reorder/bench_eval.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
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
};

struct FuzzProgram
{
    std::string name;
    AssembledProgram program;
    Seed declared; // the inputs the program's header declares (empty for generated programs)
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

// Optional hook to change the reordered words before fuzzing (the negative control uses it).
using ReorderTransform = std::function<std::vector<uint32_t>(const FuzzProgram &, PipelineKind,
                                                             std::vector<uint32_t>)>;

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
        const RunResult after = runProgram<VM>(reordered, seed, options.cycleCap);
        if (after.hitCap)
        {
            ++stats.capHits;
            ++report.capHitsByProgram[p.name];
            continue;
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
        programs.push_back({filter[u.filterIndex].name, u.program, u.seed});
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
        std::istringstream stream(makeRandomStraightLineProgram(rng, lenDist(rng)));
        programs.push_back({"random#" + std::to_string(i), assemble(stream), {}});
    }
    return programs;
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
    return out.str();
}
} // namespace Kites::fuzz
