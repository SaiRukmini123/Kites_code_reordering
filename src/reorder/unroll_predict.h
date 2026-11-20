#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/path_replay.h"

namespace Kites::reorder
{

// Loop unrolling, part 1 of 2 (predict): which loops can be unrolled, and how
// many cycles unrolling each would save, predicted from one traced run of the
// program (tracePath() in path_replay.h). Nothing in the existing pipeline
// calls this; loop_unroll.h uses it when unrolling is asked for.
//
// An eligible loop is one basic block that ends in a conditional branch back
// to its own first instruction, with at least one instruction before that
// branch and no auipc, jal, jalr or ecall inside (so no la, call, j or ret).
//
// Unrolled k times with the exit test kept in every copy:
//     L:  body
//         <inverted branch> exit      (after copies 1 .. k-1)
//         body
//         <original branch> L         (after copy k)
//     exit:
// After each entry from above, iteration i runs in copy ((i - 1) mod k) + 1:
//  - continuing from copy c < k falls through the inverted test: no flush, and
//    the hazard window carries into the next copy;
//  - continuing from copy k takes the back-branch: 3 flush cycles, empty window;
//  - leaving from copy c < k takes the inverted test: 3 flush cycles, empty window;
//  - leaving from copy k falls through the back-branch, as the loop does now.
// Every other instruction follows replayPath()'s rules, so k = 1 gives exactly
// replayPath()'s cycle count. The inverted branch reads the same registers as
// the original one, so the hazard check sees no difference between them.

inline bool isEligibleLoop(const std::vector<uint32_t> &words, const Block &block)
{
    if (block.end > words.size() || block.end < block.begin + 2)
    {
        return false;   // needs a body and a back-branch
    }
    const size_t last = block.end - 1;
    if (decode(words[last]).kind != Kind::Branch)
    {
        return false;
    }
    const int32_t offset = branchImm(words[last]);
    if (offset % 4 != 0 || static_cast<int64_t>(last) + offset / 4 != static_cast<int64_t>(block.begin))
    {
        return false;
    }
    for (size_t i = block.begin; i < last; ++i)
    {
        const Kind kind = decode(words[i]).kind;
        const bool auipc = (words[i] & 0x7F) == 0x17;
        if (auipc || kind == Kind::Jal || kind == Kind::Jalr || kind == Kind::Ecall)
        {
            return false;
        }
    }
    return true;
}

// Indices (into `blocks`) of the eligible loops, in program order.
inline std::vector<size_t> eligibleLoops(const std::vector<uint32_t> &words, const std::vector<Block> &blocks)
{
    std::vector<size_t> loops;
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        if (isEligibleLoop(words, blocks[b]))
        {
            loops.push_back(b);
        }
    }
    return loops;
}

struct UnrollPrediction
{
    uint64_t cycles = 0;        // retired + 4 + stallCycles + flushCycles
    uint64_t stallCycles = 0;   // stalls of instructions that retire
    uint64_t flushCycles = 0;   // 3 per taken branch, 2 per jump
};

// Cycles of the program in `words` (the traced program, or any reordering
// inside its blocks) as if every loop in `loops` (indices into `blocks`, each
// eligible) were unrolled k times, every copy in that block's order in
// `words`. `path` is the traced run of the original program; unrolling does
// not change which instructions run, only where the flushes fall.
inline UnrollPrediction predictUnrolled(const std::vector<uint32_t> &words, const std::vector<Block> &blocks,
                                        const ExecutedPath &path, PipelineKind kind,
                                        const std::vector<size_t> &loops, int k)
{
    std::vector<DecodedInst> insts;
    insts.reserve(words.size());
    for (uint32_t w : words)
    {
        insts.push_back(decode(w));
    }
    std::vector<bool> loopHead(words.size(), false);
    std::vector<bool> loopBranch(words.size(), false);
    for (size_t b : loops)
    {
        if (b < blocks.size() && isEligibleLoop(words, blocks[b]))
        {
            loopHead[blocks[b].begin] = true;
            loopBranch[blocks[b].end - 1] = true;
        }
    }

    UnrollPrediction r;
    WindowState window{};
    int copy = 0;   // copy the current iteration runs in; 0 = not inside an unrolled loop
    for (size_t t = 0; t < path.indices.size(); ++t)
    {
        const size_t index = path.indices[t];
        if (t > 0)
        {
            const size_t prev = path.indices[t - 1];
            if (loopBranch[prev])
            {
                if (path.taken[t - 1])   // another iteration
                {
                    if (copy < k)
                    {
                        ++copy;   // falls through the inverted test into the next copy
                    }
                    else
                    {
                        r.flushCycles += 3;   // the back-branch from the last copy
                        window = WindowState{};
                        copy = 1;
                    }
                }
                else   // leaves the loop
                {
                    if (copy < k)
                    {
                        r.flushCycles += 3;   // the inverted test jumps to the exit label
                        window = WindowState{};
                    }
                    copy = 0;
                }
            }
            else if (insts[prev].is_jump)
            {
                r.flushCycles += 2;
                window = WindowState{};
            }
            else if (insts[prev].is_branch && path.taken[t - 1])
            {
                r.flushCycles += 3;
                window = WindowState{};
            }
        }
        if (loopHead[index] && copy == 0)
        {
            copy = 1;   // entered from above
        }
        r.stallCycles += static_cast<uint64_t>(issue(window, insts[index], kind));
    }
    r.cycles = path.indices.size() + 4 + r.stallCycles + r.flushCycles;
    return r;
}

// The unrolling factor chosen for one eligible loop.
struct UnrollChoice
{
    size_t block = 0;       // index into the blocks
    int k = 1;              // 2 or 4; 1 when neither saves a cycle
    int64_t saving2 = 0;    // predicted cycles saved at k = 2
    int64_t saving4 = 0;    // predicted cycles saved at k = 4

    int64_t saving() const { return k == 2 ? saving2 : k == 4 ? saving4 : 0; }
};

// For each eligible loop on its own: the predicted saving at k = 2 and k = 4
// over the program in `words` as it is. The larger saving wins, the smaller k
// on a tie, and k = 1 (not unrolled) when neither saves anything.
inline std::vector<UnrollChoice> chooseUnrolling(const std::vector<uint32_t> &words, const std::vector<Block> &blocks,
                                                 const ExecutedPath &path, PipelineKind kind)
{
    const int64_t base = static_cast<int64_t>(predictUnrolled(words, blocks, path, kind, {}, 1).cycles);
    std::vector<UnrollChoice> choices;
    for (size_t b : eligibleLoops(words, blocks))
    {
        UnrollChoice c;
        c.block = b;
        c.saving2 = base - static_cast<int64_t>(predictUnrolled(words, blocks, path, kind, {b}, 2).cycles);
        c.saving4 = base - static_cast<int64_t>(predictUnrolled(words, blocks, path, kind, {b}, 4).cycles);
        if (c.saving4 > c.saving2 && c.saving4 > 0)
        {
            c.k = 4;
        }
        else if (c.saving2 > 0)
        {
            c.k = 2;
        }
        choices.push_back(c);
    }
    return choices;
}

} // namespace Kites::reorder
