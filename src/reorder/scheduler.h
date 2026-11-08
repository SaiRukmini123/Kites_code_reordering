#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/dep_graph.h"

namespace Kites::reorder
{

// Longest chain of instructions from i to a sink of the block's dependency
// graph, counting i itself. buildDepGraph only ever adds an edge from a
// lower local index to a higher one, so walking indices from high to low is
// already a valid topological order -- every successor of i is already done.
inline std::vector<int> criticalPathLengths(const DepGraph &g)
{
    std::vector<int> cp(g.size, 0);
    for (size_t i = g.size; i-- > 0;)
    {
        int best = 0;
        for (size_t j : g.succ[i])
        {
            best = std::max(best, cp[j]);
        }
        cp[i] = 1 + best;
    }
    return cp;
}

// Greedy list scheduler for ONE block. Repeatedly picks, among instructions
// whose predecessors (per buildDepGraph) are already placed, the one that
// adds the fewest predicted stall cycles if placed next. Ties break by:
// longer critical path first, then a load wins, then original order.
//
// Returns a permutation of local indices 0..size-1: result[k] is the
// original local index placed at new position k. If the result is not
// STRICTLY better than the original order by predictStalls, the identity
// permutation is returned instead -- scheduleBlock never makes a block worse.
inline std::vector<size_t> scheduleBlock(const std::vector<uint32_t> &words, const Block &block,
                                          PipelineKind kind, HeadContext ctx = {},
                                          bool memDisambiguation = true)
{
    const size_t n = block.end - block.begin;
    std::vector<size_t> identity(n);
    std::iota(identity.begin(), identity.end(), 0);
    if (n <= 1)
    {
        return identity;
    }

    DepGraph g = buildDepGraph(words, block, memDisambiguation);
    std::vector<int> cp = criticalPathLengths(g);

    std::vector<DecodedInst> insts;
    insts.reserve(n);
    for (size_t k = block.begin; k < block.end; ++k)
    {
        insts.push_back(decode(words[k]));
    }

    std::vector<size_t> remainingPreds(n);
    for (size_t i = 0; i < n; ++i)
    {
        remainingPreds[i] = g.pred[i].size();
    }

    std::vector<size_t> readyList;
    for (size_t i = 0; i < n; ++i)
    {
        if (remainingPreds[i] == 0)
        {
            readyList.push_back(i);
        }
    }

    std::vector<size_t> order;
    order.reserve(n);
    std::vector<DecodedInst> placed;
    placed.reserve(n);

    while (order.size() < n)
    {
        size_t bestPos = readyList.size();
        int bestStalls = 0;

        for (size_t r = 0; r < readyList.size(); ++r)
        {
            size_t cand = readyList[r];
            std::vector<DecodedInst> trial = placed;
            trial.push_back(insts[cand]);
            int stalls = predictStalls(trial, kind, ctx);

            bool better = (bestPos == readyList.size());
            if (!better)
            {
                size_t curBest = readyList[bestPos];
                if (stalls != bestStalls)
                {
                    better = stalls < bestStalls;
                }
                else if (cp[cand] != cp[curBest])
                {
                    better = cp[cand] > cp[curBest];
                }
                else if (insts[cand].is_load != insts[curBest].is_load)
                {
                    better = insts[cand].is_load;
                }
                else
                {
                    better = cand < curBest;
                }
            }

            if (better)
            {
                bestPos = r;
                bestStalls = stalls;
            }
        }

        size_t chosen = readyList[bestPos];
        readyList.erase(readyList.begin() + static_cast<std::ptrdiff_t>(bestPos));
        order.push_back(chosen);
        placed.push_back(insts[chosen]);

        for (size_t succ : g.succ[chosen])
        {
            if (--remainingPreds[succ] == 0)
            {
                readyList.push_back(succ);
            }
        }
    }

    int originalStalls = predictStalls(insts, kind, ctx);
    int scheduledStalls = predictStalls(placed, kind, ctx);
    return (scheduledStalls < originalStalls) ? order : identity;
}

// Picks out the words of one block in a given local-index order.
inline std::vector<uint32_t> materializeBlockOrder(const std::vector<uint32_t> &words,
                                                     const Block &block,
                                                     const std::vector<size_t> &order)
{
    std::vector<uint32_t> result;
    result.reserve(order.size());
    for (size_t localIdx : order)
    {
        result.push_back(words[block.begin + localIdx]);
    }
    return result;
}

struct Move
{
    size_t originalIndex = 0;
    size_t newIndex = 0;
};

struct ScheduleReport
{
    std::vector<uint32_t> words;
    std::vector<Move> moves;       // only instructions whose index actually changed
    int stallsBefore = 0;
    int stallsAfter = 0;
};

// Applies scheduleBlock to every block found via findBlocks(words,
// extraLeaders), in program order, and reassembles the full word vector.
//
// HeadContext for each block is threaded from the two instructions
// immediately before it in the WORD ARRAY -- i.e. program-array order, not
// real control-flow predecessors. For a block that is a branch target
// reached from elsewhere, or reached by fall-through from a conditional
// branch that might not be taken, this is only an approximation: the
// instruction that really executes just before it at runtime may not be
// whichever block sits immediately before it in the array. We accept that
// here because it is cheap and always available; the real VM measurement
// (instructions_retired_, total_stalls_, final registers and memory) is the
// final judge of correctness and benefit, not this prediction.
inline ScheduleReport scheduleProgram(const std::vector<uint32_t> &words, PipelineKind kind,
                                       const std::vector<size_t> &extraLeaders = {},
                                       bool memDisambiguation = true)
{
    ScheduleReport report;
    report.words.reserve(words.size());

    std::vector<Block> blocks = findBlocks(words, extraLeaders);

    DecodedInst schedPrev1{}, schedPrev2{};
    bool haveSchedPrev1 = false, haveSchedPrev2 = false;

    DecodedInst origPrev1{}, origPrev2{};
    bool haveOrigPrev1 = false, haveOrigPrev2 = false;

    for (const Block &block : blocks)
    {
        HeadContext scheduledCtx;
        if (haveSchedPrev1)
        {
            scheduledCtx.prev1 = &schedPrev1;
        }
        if (haveSchedPrev2)
        {
            scheduledCtx.prev2 = &schedPrev2;
        }

        HeadContext originalCtx;
        if (haveOrigPrev1)
        {
            originalCtx.prev1 = &origPrev1;
        }
        if (haveOrigPrev2)
        {
            originalCtx.prev2 = &origPrev2;
        }

        std::vector<DecodedInst> originalInsts;
        originalInsts.reserve(block.end - block.begin);
        for (size_t k = block.begin; k < block.end; ++k)
        {
            originalInsts.push_back(decode(words[k]));
        }
        report.stallsBefore += predictStalls(originalInsts, kind, originalCtx);

        std::vector<size_t> order = scheduleBlock(words, block, kind, scheduledCtx, memDisambiguation);
        std::vector<uint32_t> newBlockWords = materializeBlockOrder(words, block, order);

        size_t outBase = report.words.size();
        for (size_t k = 0; k < order.size(); ++k)
        {
            size_t originalIndex = block.begin + order[k];
            size_t newIndex = outBase + k;
            if (originalIndex != newIndex)
            {
                report.moves.push_back({originalIndex, newIndex});
            }
            report.words.push_back(words[originalIndex]);
        }

        std::vector<DecodedInst> scheduledInsts;
        scheduledInsts.reserve(newBlockWords.size());
        for (uint32_t w : newBlockWords)
        {
            scheduledInsts.push_back(decode(w));
        }
        report.stallsAfter += predictStalls(scheduledInsts, kind, scheduledCtx);

        if (!scheduledInsts.empty())
        {
            if (scheduledInsts.size() >= 2)
            {
                schedPrev2 = scheduledInsts[scheduledInsts.size() - 2];
                haveSchedPrev2 = true;
            }
            else
            {
                schedPrev2 = schedPrev1;
                haveSchedPrev2 = haveSchedPrev1;
            }
            schedPrev1 = scheduledInsts.back();
            haveSchedPrev1 = true;
        }

        if (!originalInsts.empty())
        {
            if (originalInsts.size() >= 2)
            {
                origPrev2 = originalInsts[originalInsts.size() - 2];
                haveOrigPrev2 = true;
            }
            else
            {
                origPrev2 = origPrev1;
                haveOrigPrev2 = haveOrigPrev1;
            }
            origPrev1 = originalInsts.back();
            haveOrigPrev1 = true;
        }
    }

    return report;
}

} // namespace Kites::reorder
