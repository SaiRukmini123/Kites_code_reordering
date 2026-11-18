#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "reorder/program_runner.h"
#include "reorder/scheduler.h"

namespace Kites::reorder
{

// One block whose order changed, tried on the VM.
struct SafetyNetTrial
{
    size_t blockIndex = 0;        // index into ScheduleReport::blocks
    Block block{};
    unsigned cyclesBefore = 0;    // whole-program cycles with the blocks accepted so far
    unsigned cyclesWith = 0;      // ... plus this block's new order
    bool accepted = false;
    enum class Reason : uint8_t { Accepted, Slower, HitCap, DifferentState } reason = Reason::Accepted;
};

struct SafetyNetResult
{
    std::vector<uint32_t> words;          // the original program with only the accepted block orders
    unsigned originalCycles = 0;
    unsigned finalCycles = 0;
    bool originalHitCap = false;          // nothing can be judged; nothing is accepted
    std::vector<SafetyNetTrial> trials;   // one per changed block, in program order

    std::vector<size_t> rejectedBlocks() const
    {
        std::vector<size_t> rejected;
        for (const SafetyNetTrial &t : trials)
        {
            if (!t.accepted)
            {
                rejected.push_back(t.blockIndex);
            }
        }
        return rejected;
    }
};

inline const char *safetyNetReasonName(SafetyNetTrial::Reason r)
{
    switch (r)
    {
    case SafetyNetTrial::Reason::Accepted: return "accepted";
    case SafetyNetTrial::Reason::Slower: return "slower";
    case SafetyNetTrial::Reason::HitCap: return "hit the cycle cap";
    case SafetyNetTrial::Reason::DifferentState: return "different final state";
    }
    return "?";
}

// Starts from the original program and, block by block in program order,
// keeps a block's new order (from `report`) only if the whole program, run on
// VM with `seed`, finishes under `cycleCap`, ends in the same state as the
// original (sameState: registers, data-section window, stack window) and
// takes no more cycles than with the blocks accepted so far. Every accepted
// step is non-increasing, so the result is never slower than the original.
// Costs one VM run per changed block, plus one for the original.
template <class VM>
SafetyNetResult applyWithVmCheck(const Kites::AssembledProgram &program, const ScheduleReport &report,
                                 const Kites::Seed &seed = {}, unsigned cycleCap = 100000)
{
    const std::vector<uint32_t> &original = program.text_buffer;
    SafetyNetResult result;
    result.words = original;

    const Kites::RunResult base = Kites::runProgram<VM>(program, seed, cycleCap);
    result.originalCycles = base.cycles;
    result.finalCycles = base.cycles;
    result.originalHitCap = base.hitCap;
    if (base.hitCap)
    {
        return result;
    }

    for (size_t b = 0; b < report.blocks.size(); ++b)
    {
        const Block &block = report.blocks[b].block;
        bool changed = false;
        for (size_t k = block.begin; k < block.end; ++k)
        {
            changed = changed || report.words[k] != original[k];
        }
        if (!changed)
        {
            continue;
        }

        std::vector<uint32_t> candidate = result.words;
        for (size_t k = block.begin; k < block.end; ++k)
        {
            candidate[k] = report.words[k];
        }
        const Kites::RunResult run = Kites::runProgram<VM>(Kites::withText(program, candidate), seed, cycleCap);

        SafetyNetTrial trial;
        trial.blockIndex = b;
        trial.block = block;
        trial.cyclesBefore = result.finalCycles;
        trial.cyclesWith = run.cycles;
        if (run.hitCap)
        {
            trial.reason = SafetyNetTrial::Reason::HitCap;
        }
        else if (!Kites::sameState(base, run))
        {
            trial.reason = SafetyNetTrial::Reason::DifferentState;
        }
        else if (run.cycles > result.finalCycles)
        {
            trial.reason = SafetyNetTrial::Reason::Slower;
        }
        else
        {
            trial.accepted = true;
            result.words = candidate;
            result.finalCycles = run.cycles;
        }
        result.trials.push_back(trial);
    }
    return result;
}

} // namespace Kites::reorder
