#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/block_optimizer.h"
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

// ======================================================================
// Exhaustive (branch-and-bound) scheduling of one block. Stable interface:
// scheduleBlockOptimal(), scheduleBlockOptimalDetailed(), OptimalBlockResult,
// OptimizerMethod and the two constants below.
// ======================================================================

// Largest block the search attempts: placed-instruction sets are 64-bit masks.
inline constexpr size_t kMaxOptimalBlockSize = 64;

// Default cap on search positions expanded per block. Each position costs at
// most one issue() per instruction plus one memo entry, so this bounds both
// time and memory for every block size; reorder_optimal_test measures the
// time of a block that hits the cap.
inline constexpr uint64_t kDefaultOptimalNodeBudget = 200000;

// How a block's order was chosen.
enum class OptimizerMethod : uint8_t
{
    NotSearched,         // ScheduleMode::Greedy: greedy order, the search never ran
    Optimal,             // search finished: fewest predicted stalls of all legal orders
    GreedyAlreadyZero,   // greedy already had 0 stalls, so it is optimal; nothing searched
    BestFound,           // node cap hit; best order found beats greedy, NOT proven optimal
    GreedyCapHit,        // node cap hit before anything beat greedy; greedy order, NOT proven optimal
    GreedyTooLarge,      // more than kMaxOptimalBlockSize instructions; greedy order, not searched
};

inline const char *optimizerMethodName(OptimizerMethod method)
{
    switch (method)
    {
    case OptimizerMethod::NotSearched: return "greedy (not searched)";
    case OptimizerMethod::Optimal: return "optimal";
    case OptimizerMethod::GreedyAlreadyZero: return "optimal (greedy already 0)";
    case OptimizerMethod::BestFound: return "best found, not proven optimal";
    case OptimizerMethod::GreedyCapHit: return "greedy (node cap hit, not proven optimal)";
    case OptimizerMethod::GreedyTooLarge: return "greedy (block too large to search)";
    }
    return "?";
}

// Result of scheduleBlockOptimalDetailed() for one block. All stall counts
// are predictStalls() for that block with the same HeadContext.
struct OptimalBlockResult
{
    // The chosen order, like scheduleBlock(): order[k] is the original local
    // index placed at position k. Always a legal order of the dependency graph.
    std::vector<size_t> order;

    // Predicted stalls of `order`. The minimum over all legal orders when
    // provenOptimal(); otherwise just the best this run found.
    int optimalStalls = 0;

    // Predicted stalls of scheduleBlock()'s (greedy) order for the same block.
    // optimalStalls <= greedyStalls always.
    int greedyStalls = 0;

    // Predicted stalls of the block as written.
    int originalStalls = 0;

    OptimizerMethod method = OptimizerMethod::Optimal;

    // Search positions expanded (0 when nothing was searched).
    uint64_t nodesSearched = 0;

    bool provenOptimal() const
    {
        return method == OptimizerMethod::Optimal || method == OptimizerMethod::GreedyAlreadyZero;
    }
};

// Like scheduleBlock(), but returns an order with the fewest predicted stalls
// among ALL legal orders of the block, found by branch-and-bound search (see
// OrderSearch in block_optimizer.h) started from the greedy order.
//
// - If nothing beats greedy, greedy's order is returned (so the keep-original
//   rule of scheduleBlock() still applies: the original order is kept unless
//   a strictly better one exists).
// - If a better order is found and proven optimal, the one that is smallest
//   as a sequence of original indices is returned.
// - Blocks over kMaxOptimalBlockSize, or searches that expand more than
//   nodeBudget positions, are reported through `method`; a capped search
//   still returns the best order it found if that beats greedy.
inline OptimalBlockResult scheduleBlockOptimalDetailed(const std::vector<uint32_t> &words,
                                                       const Block &block, PipelineKind kind,
                                                       HeadContext ctx = {},
                                                       bool memDisambiguation = true,
                                                       uint64_t nodeBudget = kDefaultOptimalNodeBudget)
{
    const size_t n = block.end - block.begin;
    std::vector<DecodedInst> insts;
    insts.reserve(n);
    for (size_t k = block.begin; k < block.end; ++k)
    {
        insts.push_back(decode(words[k]));
    }

    OptimalBlockResult result;
    result.originalStalls = predictStalls(insts, kind, ctx);
    result.order = scheduleBlock(words, block, kind, ctx, memDisambiguation);
    std::vector<DecodedInst> greedyInsts;
    greedyInsts.reserve(n);
    for (size_t localIdx : result.order)
    {
        greedyInsts.push_back(insts[localIdx]);
    }
    result.greedyStalls = predictStalls(greedyInsts, kind, ctx);
    result.optimalStalls = result.greedyStalls;

    if (n <= 1)
    {
        result.method = OptimizerMethod::Optimal;
        return result;
    }
    if (result.greedyStalls == 0)
    {
        result.method = OptimizerMethod::GreedyAlreadyZero;
        return result;
    }
    if (n > kMaxOptimalBlockSize)
    {
        result.method = OptimizerMethod::GreedyTooLarge;
        return result;
    }

    const DepGraph g = buildDepGraph(words, block, memDisambiguation);
    const std::vector<int> cp = criticalPathLengths(g);
    OrderSearch search(insts, g, cp, kind, nodeBudget);
    const OrderSearchResult found = search.run(ctx, result.order, result.greedyStalls);

    result.order = found.order;
    result.optimalStalls = found.stalls;
    result.nodesSearched = found.nodes;
    if (found.proven)
    {
        result.method = OptimizerMethod::Optimal;
    }
    else
    {
        result.method = (found.stalls < result.greedyStalls) ? OptimizerMethod::BestFound
                                                              : OptimizerMethod::GreedyCapHit;
    }
    return result;
}

// The order alone, with the same signature and meaning as scheduleBlock().
inline std::vector<size_t> scheduleBlockOptimal(const std::vector<uint32_t> &words, const Block &block,
                                                PipelineKind kind, HeadContext ctx = {},
                                                bool memDisambiguation = true)
{
    return scheduleBlockOptimalDetailed(words, block, kind, ctx, memDisambiguation).order;
}

struct Move
{
    size_t originalIndex = 0;
    size_t newIndex = 0;
};

struct BlockStalls
{
    Block block{};
    int stallsBefore = 0;
    int stallsAfter = 0;
    int greedyStalls = 0;      // greedy order's predicted stalls (same head context)
    int optimalStalls = -1;    // ScheduleMode::Optimal only; -1 when not searched
    OptimizerMethod method = OptimizerMethod::NotSearched;
    uint64_t nodesSearched = 0;
};

enum class ScheduleMode : uint8_t
{
    Greedy,    // scheduleBlock() for every block (the default)
    Optimal,   // scheduleBlockOptimalDetailed() for every block
};

struct ScheduleReport
{
    std::vector<uint32_t> words;
    std::vector<Move> moves;       // only instructions whose index actually changed
    std::vector<BlockStalls> blocks;
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
                                       bool memDisambiguation = true,
                                       ScheduleMode mode = ScheduleMode::Greedy)
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
        BlockStalls blockStalls;
        blockStalls.block = block;
        blockStalls.stallsBefore = predictStalls(originalInsts, kind, originalCtx);
        report.stallsBefore += blockStalls.stallsBefore;

        std::vector<size_t> order;
        if (mode == ScheduleMode::Optimal)
        {
            OptimalBlockResult optimal =
                scheduleBlockOptimalDetailed(words, block, kind, scheduledCtx, memDisambiguation);
            order = std::move(optimal.order);
            blockStalls.greedyStalls = optimal.greedyStalls;
            blockStalls.optimalStalls = optimal.optimalStalls;
            blockStalls.method = optimal.method;
            blockStalls.nodesSearched = optimal.nodesSearched;
        }
        else
        {
            order = scheduleBlock(words, block, kind, scheduledCtx, memDisambiguation);
        }
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
        blockStalls.stallsAfter = predictStalls(scheduledInsts, kind, scheduledCtx);
        if (mode == ScheduleMode::Greedy)
        {
            blockStalls.greedyStalls = blockStalls.stallsAfter;
        }
        report.stallsAfter += blockStalls.stallsAfter;
        report.blocks.push_back(blockStalls);

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

// Greedy-vs-optimal totals of a ScheduleMode::Optimal report. Each block's
// greedy and optimal numbers use the same head context.
struct OptimalSummary
{
    int greedyStalls = 0;
    int optimalStalls = 0;
    size_t blocks = 0;
    size_t provenBlocks = 0;            // Optimal or GreedyAlreadyZero
    size_t greedyAlreadyOptimal = 0;    // proven, and optimal == greedy
    size_t improvedBlocks = 0;          // optimal < greedy (proven or best found)
    size_t bestFoundBlocks = 0;
    size_t capHitBlocks = 0;
    size_t tooLargeBlocks = 0;
    uint64_t nodesSearched = 0;

    int gap() const { return greedyStalls - optimalStalls; }
};

inline OptimalSummary summarizeOptimal(const ScheduleReport &report)
{
    OptimalSummary s;
    for (const BlockStalls &b : report.blocks)
    {
        ++s.blocks;
        s.greedyStalls += b.greedyStalls;
        s.optimalStalls += b.stallsAfter;
        s.nodesSearched += b.nodesSearched;
        const bool proven = b.method == OptimizerMethod::Optimal || b.method == OptimizerMethod::GreedyAlreadyZero;
        s.provenBlocks += proven ? 1 : 0;
        s.greedyAlreadyOptimal += (proven && b.stallsAfter == b.greedyStalls) ? 1 : 0;
        s.improvedBlocks += (b.stallsAfter < b.greedyStalls) ? 1 : 0;
        s.bestFoundBlocks += (b.method == OptimizerMethod::BestFound) ? 1 : 0;
        s.capHitBlocks += (b.method == OptimizerMethod::GreedyCapHit) ? 1 : 0;
        s.tooLargeBlocks += (b.method == OptimizerMethod::GreedyTooLarge) ? 1 : 0;
    }
    return s;
}

} // namespace Kites::reorder
