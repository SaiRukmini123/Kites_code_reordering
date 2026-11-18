#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "reorder/block_entries.h"
#include "reorder/block_finder.h"
#include "reorder/block_optimizer.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/dep_graph.h"
#include "reorder/pinned_slots.h"

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
//
// Pinned instructions (auipc) keep their slot: at a pinned slot the pinned
// instruction is placed (it is always ready by then); at any other slot only
// unpinned instructions that still leave a legal way to finish the block
// (PinnedSlots::canComplete) are candidates. The original order is one such
// way, and every choice keeps one open, so greedy can never get stuck: when
// an instruction that must precede an upcoming pin runs out of slack, it is
// the only candidate left. Several pins in a block are handled the same way.
// Blocks without pinned instructions are scheduled exactly as before.
//
// This overload scores orders with a BlockScoring (several weighted head
// windows and a tail; see cost_model.h): each candidate costs the weighted
// stalls of the partial order under every head. When the scoring has a tail,
// the last free choice (two instructions left) compares the two complete
// endings including the tail. Keep-original compares full scores.
inline std::vector<size_t> scheduleBlock(const std::vector<uint32_t> &words, const Block &block,
                                          PipelineKind kind, const BlockScoring &scoring,
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

    const PinnedSlots pins(insts, g);
    std::vector<bool> isPlaced(n, false);

    while (order.size() < n)
    {
        const size_t slot = order.size();
        size_t bestPos = readyList.size();
        int bestStalls = 0;

        for (size_t r = 0; r < readyList.size(); ++r)
        {
            size_t cand = readyList[r];
            if (pins.any)
            {
                if (pins.pinned[slot] ? cand != slot : pins.pinned[cand])
                {
                    continue;
                }
                isPlaced[cand] = true;
                const bool keepsAWayOut = pins.pinned[slot] || pins.canComplete(isPlaced, slot + 1, g);
                isPlaced[cand] = false;
                if (!keepsAWayOut)
                {
                    continue;
                }
            }
            std::vector<DecodedInst> trial = placed;
            trial.push_back(insts[cand]);
            int stalls = 0;
            if (!scoring.tail.empty() && slot + 2 == n)
            {
                for (size_t other = 0; other < n; ++other)
                {
                    if (!isPlaced[other] && other != cand)
                    {
                        trial.push_back(insts[other]);   // the only instruction left
                    }
                }
                stalls = scoreOrder(trial, kind, scoring, true);
            }
            else
            {
                stalls = scoreOrder(trial, kind, scoring, false);
            }

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

        if (bestPos == readyList.size())
        {
            // Only reachable if canComplete() were wrong; never return an
            // order that moves a pinned instruction.
            return identity;
        }
        size_t chosen = readyList[bestPos];
        readyList.erase(readyList.begin() + static_cast<std::ptrdiff_t>(bestPos));
        order.push_back(chosen);
        placed.push_back(insts[chosen]);
        isPlaced[chosen] = true;

        for (size_t succ : g.succ[chosen])
        {
            if (--remainingPreds[succ] == 0)
            {
                readyList.push_back(succ);
            }
        }
    }

    const int originalScore = scoreOrder(insts, kind, scoring);
    const int scheduledScore = scoreOrder(placed, kind, scoring);
    return (scheduledScore < originalScore) ? order : identity;
}

// One head context, weight 1, no tail: exactly predictStalls() with `ctx`.
inline std::vector<size_t> scheduleBlock(const std::vector<uint32_t> &words, const Block &block,
                                          PipelineKind kind, HeadContext ctx = {},
                                          bool memDisambiguation = true)
{
    return scheduleBlock(words, block, kind, singleContext(ctx), memDisambiguation);
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
// - Pinned instructions (auipc) keep their original slot, as in scheduleBlock().
// - Blocks over kMaxOptimalBlockSize, or searches that expand more than
//   nodeBudget positions, are reported through `method`; a capped search
//   still returns the best order it found if that beats greedy.
//
// This overload scores orders with a BlockScoring (1 or 2 weighted head
// windows and a weighted tail; see cost_model.h and block_entries.h). Here
// the OptimalBlockResult fields optimalStalls, greedyStalls and
// originalStalls hold that WEIGHTED SCORE, not a plain stall count. The
// single-HeadContext overload below is the same thing with one head of
// weight 1 and no tail, so there they are plain predicted stalls.
inline OptimalBlockResult scheduleBlockOptimalDetailed(const std::vector<uint32_t> &words,
                                                       const Block &block, PipelineKind kind,
                                                       const BlockScoring &scoring,
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
    result.originalStalls = scoreOrder(insts, kind, scoring);
    result.order = scheduleBlock(words, block, kind, scoring, memDisambiguation);
    std::vector<DecodedInst> greedyInsts;
    greedyInsts.reserve(n);
    for (size_t localIdx : result.order)
    {
        greedyInsts.push_back(insts[localIdx]);
    }
    result.greedyStalls = scoreOrder(greedyInsts, kind, scoring);
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
    if (n > kMaxOptimalBlockSize || scoring.heads.empty() || scoring.heads.size() > OrderSearch::kMaxHeads)
    {
        result.method = OptimizerMethod::GreedyTooLarge;
        return result;
    }

    const DepGraph g = buildDepGraph(words, block, memDisambiguation);
    const std::vector<int> cp = criticalPathLengths(g);
    OrderSearch search(insts, g, cp, kind, nodeBudget);
    const OrderSearchResult found = search.run(scoring, result.order, result.greedyStalls);

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

// One head context, weight 1, no tail: plain predicted stalls (the original
// interface, unchanged).
inline OptimalBlockResult scheduleBlockOptimalDetailed(const std::vector<uint32_t> &words,
                                                       const Block &block, PipelineKind kind,
                                                       HeadContext ctx = {},
                                                       bool memDisambiguation = true,
                                                       uint64_t nodeBudget = kDefaultOptimalNodeBudget)
{
    return scheduleBlockOptimalDetailed(words, block, kind, singleContext(ctx), memDisambiguation, nodeBudget);
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

    // Plain predicted stalls of the block: no weights and no tail, from the
    // one context the block is predicted with -- the two instructions above it
    // if it can be entered by falling through from the block above (always, in
    // ContextMode::ArrayOrder), otherwise an empty window (it is only reached
    // by a taken branch or jump, a call's return, or program start).
    // stallsBefore: the original order in the original program.
    // stallsAfter: the new order in the scheduled program.
    // Predicted with the same HazardModel as the scores.
    int stallsBefore = 0;
    int stallsAfter = 0;

    // Scores, in the same units as scoreAfter (not plain stalls).
    int greedyStalls = 0;      // greedy order's score
    int optimalStalls = -1;    // ScheduleMode::Optimal only: the chosen order's score; -1 when not searched
    OptimizerMethod method = OptimizerMethod::NotSearched;
    uint64_t nodesSearched = 0;
    BlockEntry entry{};        // ContextMode::BlockEntries only: how the block can be entered
    int tailWeight = 0;        // ContextMode::BlockEntries only: weight of the tail term (0 = no tail)

    // The weighted objective the scheduler minimizes (BlockScoring): each
    // entry context's stalls times its edge weight, plus the weighted tail.
    // In ContextMode::ArrayOrder it equals stallsBefore / stallsAfter.
    int scoreBefore = 0;
    int scoreAfter = 0;
};

enum class ScheduleMode : uint8_t
{
    Greedy,    // scheduleBlock() for every block (the default)
    Optimal,   // scheduleBlockOptimalDetailed() for every block
};

enum class ContextMode : uint8_t
{
    BlockEntries,   // score each block by how it can be entered, plus its tail (the default)
    ArrayOrder,     // the previous behaviour: one context, the two instructions above in the array
};

struct ScheduleReport
{
    std::vector<uint32_t> words;
    std::vector<Move> moves;       // only instructions whose index actually changed
    std::vector<BlockStalls> blocks;
    int stallsBefore = 0;   // sum of BlockStalls::stallsBefore (plain predicted stalls)
    int stallsAfter = 0;    // sum of BlockStalls::stallsAfter
    int scoreBefore = 0;    // sum of BlockStalls::scoreBefore (weighted objective)
    int scoreAfter = 0;     // sum of BlockStalls::scoreAfter
};

// Applies scheduleBlock (or the optimal search) to every block found via
// findBlocks(words, extraLeaders), in program order, and reassembles the
// full word vector.
//
// ContextMode::BlockEntries (default) scores each block by how it can really
// be entered (blockEntries()): falling through starts from the last two
// instructions of the block above, in their scheduled order; a taken branch,
// jal or jalr target, a label, a call's return point and program start begin
// with an empty window (measured on both VMs in reorder_boundary_test). A
// block entered both ways is scored under both, each weighted by its entry
// edge (loopWeight inside a loop, else 1). If the block can fall through, the
// stalls its order causes on the next block's first two instructions (in
// their original order; that block isn't scheduled yet) are added, weighted
// by that fall-through edge. A block that can't be entered at all (dead code)
// is scored from an empty window. With loopWeight = 1 every weight is 1.
// scoreBefore/scoreAfter hold that weighted score; stallsBefore/stallsAfter
// hold plain predicted stalls (see BlockStalls).
//
// `model` chooses the hazard model used for scoring and for the predicted
// stall fields (HazardModel::Exact, the default, is what the VM does;
// Textbook is for the quirk ablation).
//
// ContextMode::ArrayOrder is the previous behaviour: one context, the two
// instructions immediately above in the WORD ARRAY, which is only an
// approximation at branch targets. Either way the real VM measurement is the
// final judge (see vm_safety_net.h).
inline ScheduleReport scheduleProgram(const std::vector<uint32_t> &words, PipelineKind kind,
                                       const std::vector<size_t> &extraLeaders = {},
                                       bool memDisambiguation = true,
                                       ScheduleMode mode = ScheduleMode::Greedy,
                                       ContextMode contexts = ContextMode::BlockEntries,
                                       int loopWeight = kDefaultLoopWeight,
                                       HazardModel model = HazardModel::Exact)
{
    ScheduleReport report;
    report.words.reserve(words.size());

    std::vector<Block> blocks = findBlocks(words, extraLeaders);
    const std::vector<BlockEntry> entries = blockEntries(words, blocks, extraLeaders, loopWeight);

    DecodedInst schedPrev1{}, schedPrev2{};
    bool haveSchedPrev1 = false, haveSchedPrev2 = false;

    DecodedInst origPrev1{}, origPrev2{};
    bool haveOrigPrev1 = false, haveOrigPrev2 = false;

    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const Block &block = blocks[b];
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

        // How this block is scored, from a given fall-through context.
        auto scoringFrom = [&](const HeadContext &fallThroughCtx)
        {
            if (contexts == ContextMode::ArrayOrder)
            {
                BlockScoring single = singleContext(fallThroughCtx);
                single.model = model;
                return single;
            }
            const BlockEntry &entry = entries[b];
            BlockScoring scoring;
            scoring.model = model;
            if (entry.fallThrough)
            {
                scoring.heads.push_back(ScoredHead{windowFrom(fallThroughCtx), entry.fallThroughWeight});
            }
            if (entry.clean || !entry.fallThrough)
            {
                scoring.heads.push_back(ScoredHead{WindowState{}, entry.clean ? entry.cleanWeight : 1});
            }
            if (b + 1 < blocks.size() && entries[b + 1].fallThrough)
            {
                const Block &next = blocks[b + 1];
                for (size_t k = next.begin; k < next.end && k < next.begin + 2; ++k)
                {
                    scoring.tail.push_back(decode(words[k]));
                }
                scoring.tailWeight = entries[b + 1].fallThroughWeight;
            }
            return scoring;
        };
        const BlockScoring originalScoring = scoringFrom(originalCtx);
        const BlockScoring scheduledScoring = scoringFrom(scheduledCtx);
        if (contexts == ContextMode::BlockEntries)
        {
            blockStalls.entry = entries[b];
            blockStalls.tailWeight = scheduledScoring.tailWeight;
        }
        // The one context plain stalls are predicted with (see BlockStalls).
        auto plainContext = [&](const HeadContext &fallThroughCtx)
        {
            const bool fallsIn = contexts == ContextMode::ArrayOrder || entries[b].fallThrough;
            return fallsIn ? fallThroughCtx : HeadContext{};
        };
        blockStalls.stallsBefore = predictStalls(originalInsts, kind, plainContext(originalCtx), model);
        blockStalls.scoreBefore = scoreOrder(originalInsts, kind, originalScoring);
        report.stallsBefore += blockStalls.stallsBefore;
        report.scoreBefore += blockStalls.scoreBefore;

        std::vector<size_t> order;
        if (mode == ScheduleMode::Optimal)
        {
            OptimalBlockResult optimal =
                scheduleBlockOptimalDetailed(words, block, kind, scheduledScoring, memDisambiguation);
            order = std::move(optimal.order);
            blockStalls.greedyStalls = optimal.greedyStalls;
            blockStalls.optimalStalls = optimal.optimalStalls;
            blockStalls.method = optimal.method;
            blockStalls.nodesSearched = optimal.nodesSearched;
        }
        else
        {
            order = scheduleBlock(words, block, kind, scheduledScoring, memDisambiguation);
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
        blockStalls.stallsAfter = predictStalls(scheduledInsts, kind, plainContext(scheduledCtx), model);
        blockStalls.scoreAfter = scoreOrder(scheduledInsts, kind, scheduledScoring);
        if (mode == ScheduleMode::Greedy)
        {
            blockStalls.greedyStalls = blockStalls.scoreAfter;
        }
        report.stallsAfter += blockStalls.stallsAfter;
        report.scoreAfter += blockStalls.scoreAfter;
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

// Greedy-vs-optimal totals of a ScheduleMode::Optimal report, in score units
// (BlockStalls::greedyStalls and scoreAfter; the optimizer's objective, which
// equals plain stalls only in ContextMode::ArrayOrder). Each block's greedy
// and optimal numbers use the same scoring.
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
        s.optimalStalls += b.scoreAfter;
        s.nodesSearched += b.nodesSearched;
        const bool proven = b.method == OptimizerMethod::Optimal || b.method == OptimizerMethod::GreedyAlreadyZero;
        s.provenBlocks += proven ? 1 : 0;
        s.greedyAlreadyOptimal += (proven && b.scoreAfter == b.greedyStalls) ? 1 : 0;
        s.improvedBlocks += (b.scoreAfter < b.greedyStalls) ? 1 : 0;
        s.bestFoundBlocks += (b.method == OptimizerMethod::BestFound) ? 1 : 0;
        s.capHitBlocks += (b.method == OptimizerMethod::GreedyCapHit) ? 1 : 0;
        s.tooLargeBlocks += (b.method == OptimizerMethod::GreedyTooLarge) ? 1 : 0;
    }
    return s;
}

} // namespace Kites::reorder
