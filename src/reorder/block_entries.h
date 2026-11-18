#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/decoded_inst.h"

namespace Kites::reorder
{

// Default weight of control-flow edges that stay inside a loop.
inline constexpr int kDefaultLoopWeight = 10;

// A loop: the instruction range from a backward branch or jump's target to
// the branch itself, inclusive.
struct LoopRange
{
    size_t first = 0;
    size_t last = 0;
};

inline std::vector<LoopRange> findLoops(const std::vector<uint32_t> &words)
{
    std::vector<LoopRange> loops;
    for (size_t i = 0; i < words.size(); ++i)
    {
        const DecodedInst d = decode(words[i]);
        if (d.kind != Kind::Branch && d.kind != Kind::Jal)
        {
            continue;
        }
        const int32_t off = (d.kind == Kind::Branch) ? branchImm(words[i]) : jalImm(words[i]);
        const int64_t target = static_cast<int64_t>(i) + off / 4;
        if (off % 4 == 0 && target >= 0 && target <= static_cast<int64_t>(i))
        {
            loops.push_back({static_cast<size_t>(target), i});
        }
    }
    return loops;
}

// Weight of the control-flow edge from instruction `from` to instruction
// `to`: loopWeight if both lie inside the same loop (the back edge, and
// fall-throughs and branches within the loop), else 1 (loop entry, loop
// exit, and edges outside loops). Nested loops still give loopWeight once.
inline int edgeWeight(size_t from, size_t to, const std::vector<LoopRange> &loops, int loopWeight)
{
    for (const LoopRange &l : loops)
    {
        if (l.first <= from && from <= l.last && l.first <= to && to <= l.last)
        {
            return loopWeight;
        }
    }
    return 1;
}

// How a block can be entered, as measured on the VM (reorder_boundary_test,
// FlushExperiment): after a taken branch, jal or jalr the target starts with
// an empty hazard window ("clean"); falling through keeps the last two
// instructions above it (a not-taken branch counts as one of them).
struct BlockEntry
{
    bool fallThrough = false;     // the block above can run into it
    bool clean = false;           // program start, a branch/jal target, a code label
                                  // (possible jalr target) or a call's return point
    int fallThroughWeight = 0;    // weight of the fall-through edge into it
    int cleanWeight = 0;          // largest weight among its clean entries
};

inline bool isUnconditionalJump(const DecodedInst &d) { return d.kind == Kind::Jal || d.kind == Kind::Jalr; }

inline std::vector<BlockEntry> blockEntries(const std::vector<uint32_t> &words, const std::vector<Block> &blocks,
                                            const std::vector<size_t> &extraLeaders,
                                            int loopWeight = kDefaultLoopWeight)
{
    const std::vector<LoopRange> loops = findLoops(words);
    std::vector<BlockEntry> entries(blocks.size());
    std::vector<size_t> blockAt(words.size() + 1, blocks.size());
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        blockAt[blocks[b].begin] = b;
    }
    auto addClean = [&](size_t b, int weight)
    {
        entries[b].clean = true;
        entries[b].cleanWeight = std::max(entries[b].cleanWeight, weight);
    };

    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const size_t begin = blocks[b].begin;
        if (b == 0)
        {
            addClean(b, 1);   // program start: empty pipeline
            continue;
        }
        const DecodedInst above = decode(words[begin - 1]);
        if (!isUnconditionalJump(above))
        {
            entries[b].fallThrough = true;
            entries[b].fallThroughWeight = edgeWeight(begin - 1, begin, loops, loopWeight);
        }
        else if (above.writes_rd && above.rd != 0)
        {
            addClean(b, 1);   // return point of a call (reached by the callee's jalr)
        }
    }

    for (size_t idx : extraLeaders)   // code labels: possible jalr targets
    {
        if (idx < words.size() && blockAt[idx] != blocks.size())
        {
            addClean(blockAt[idx], 1);
        }
    }

    for (size_t i = 0; i < words.size(); ++i)   // taken branches and jals
    {
        const DecodedInst d = decode(words[i]);
        if (d.kind != Kind::Branch && d.kind != Kind::Jal)
        {
            continue;
        }
        const int32_t off = (d.kind == Kind::Branch) ? branchImm(words[i]) : jalImm(words[i]);
        const int64_t target = static_cast<int64_t>(i) + off / 4;
        if (off % 4 == 0 && target >= 0 && target < static_cast<int64_t>(words.size()) &&
            blockAt[static_cast<size_t>(target)] != blocks.size())
        {
            addClean(blockAt[static_cast<size_t>(target)], edgeWeight(i, static_cast<size_t>(target), loops, loopWeight));
        }
    }
    return entries;
}

} // namespace Kites::reorder
