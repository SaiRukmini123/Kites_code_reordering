#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "reorder/cost_model.h"
#include "reorder/dep_graph.h"
#include "reorder/pinned_slots.h"

namespace Kites::reorder
{

// The window with every field checkHazard() can never read for this
// pipeline cleared, so windows that always lead to the same future stalls
// share one memo entry:
//  - rv5-h-nf never looks at isLoad;
//  - rv5-h-f only looks at the newer slot, and only when it is a load that
//    writes a register (the older slot never matters).
inline WindowState canonicalWindow(const WindowState &w, PipelineKind kind)
{
    WindowState c;
    if (kind == PipelineKind::HF)
    {
        if (w.newer.writes && w.newer.isLoad)
        {
            c.newer = Producer{true, w.newer.rd, true};
        }
        return c;
    }
    if (w.older.writes)
    {
        c.older = Producer{true, w.older.rd, false};
    }
    if (w.newer.writes)
    {
        c.newer = Producer{true, w.newer.rd, false};
    }
    return c;
}

struct OrderSearchResult
{
    std::vector<size_t> order;   // best order found (local indices, like scheduleBlock)
    int stalls = 0;              // its predicted stalls
    bool proven = false;         // search finished: no legal order has fewer stalls
    uint64_t nodes = 0;          // search positions expanded
};

// Branch-and-bound search over the legal orders (topological orders of `g`)
// of one block of at most 64 instructions, for the fewest predicted stalls.
//
// A search position is (set of placed instructions, window). After an
// instruction issues, the window is all the hazard check will ever read
// again (see WindowState), so the stalls still to come depend only on the
// position, never on how it was reached. Stalls already charged never
// change as more instructions are placed, so the stalls so far are a lower
// bound on any completion.
//
// Pruning, with `best` the fewest stalls of any complete order found so far
// (starting from startOrder/startStalls):
//  - a partial order whose stalls so far are >= best is cut;
//  - a position already expanded with stalls so far <= the current ones is
//    cut. That earlier visit tried every completion that could beat the best
//    of its time, and best only goes down, so nothing new can be found here.
// The search stops as soon as best reaches 0, or after nodeBudget positions.
//
// Pinned instructions (auipc) keep their slot: a pinned slot accepts only
// its own instruction, other slots only unpinned ones, and a position where
// an unplaced instruction has missed its deadline (see PinnedSlots) is cut.
// Without pinned instructions the search is unchanged.
class OrderSearch
{
public:
    OrderSearch(const std::vector<DecodedInst> &insts, const DepGraph &g,
                const std::vector<int> &criticalPath, PipelineKind kind, uint64_t nodeBudget)
        : insts_(insts), criticalPath_(criticalPath), kind_(kind), budget_(nodeBudget),
          full_(insts.size() == 64 ? ~uint64_t{0} : (uint64_t{1} << insts.size()) - 1),
          predMask_(insts.size(), 0), pins_(insts, g)
    {
        for (size_t j = 0; j < insts.size(); ++j)
        {
            for (size_t i : g.pred[j])
            {
                predMask_[j] |= uint64_t{1} << i;
            }
        }
    }

    OrderSearchResult run(const HeadContext &ctx, const std::vector<size_t> &startOrder, int startStalls)
    {
        best_ = startStalls;
        bestOrder_ = startOrder;
        improve(0, windowFrom(ctx), 0);
        const bool improved = best_ < startStalls;

        OrderSearchResult result;
        result.proven = !aborted_;
        result.stalls = best_;
        result.order = bestOrder_;

        // A better order than the start was found and proven optimal: among
        // all orders with that many stalls, return the one that is smallest
        // when read as a sequence of original indices, so instructions move
        // as little as possible from the front of the block.
        if (improved && result.proven)
        {
            seen_.clear();
            path_.clear();
            if (firstWithStalls(0, windowFrom(ctx), 0, best_))
            {
                result.order = path_;
            }
        }
        result.nodes = nodes_;
        return result;
    }

private:
    struct Key
    {
        uint64_t placed;
        uint16_t window;
        bool operator==(const Key &o) const { return placed == o.placed && window == o.window; }
    };
    struct KeyHash
    {
        size_t operator()(const Key &k) const
        {
            uint64_t h = k.placed * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<uint64_t>(k.window) + 0x632BE59BD9B4E019ull) + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };
    struct Candidate
    {
        size_t index;
        int stalls;
        WindowState after;
    };

    Key keyOf(uint64_t placed, const WindowState &w) const
    {
        const WindowState c = canonicalWindow(w, kind_);
        auto code = [](const Producer &p)
        { return p.writes ? static_cast<uint16_t>(p.rd | (p.isLoad ? 32u : 0u)) : uint16_t{0}; };
        return Key{placed, static_cast<uint16_t>(code(c.older) | (code(c.newer) << 6))};
    }

    // Ready (all predecessors placed) and allowed at the next slot: a pinned
    // slot takes only its own pinned instruction, any other slot only
    // unpinned ones. The next slot is the number of instructions placed.
    bool ready(size_t i, uint64_t placed) const
    {
        if (((placed >> i) & 1u) != 0 || (predMask_[i] & ~placed) != 0)
        {
            return false;
        }
        if (!pins_.any)
        {
            return true;
        }
        const size_t slot = static_cast<size_t>(std::popcount(placed));
        return pins_.pinned[slot] ? i == slot : !pins_.pinned[i];
    }

    // An unplaced instruction has missed its deadline (PinnedSlots), so no
    // completion can put every pinned instruction on its slot.
    bool deadEnd(uint64_t placed) const
    {
        return pins_.any && pins_.someDeadlinePassed(static_cast<size_t>(std::popcount(placed)),
                                                     [placed](size_t i) { return ((placed >> i) & 1u) != 0; });
    }

    // Returns false (and does not count a node) when this position was
    // already expanded with stalls so far <= `stalls`; otherwise records it.
    bool firstVisitAtCost(const Key &key, int stalls)
    {
        auto [it, inserted] = seen_.try_emplace(key, stalls);
        if (!inserted)
        {
            if (it->second <= stalls)
            {
                return false;
            }
            it->second = stalls;
        }
        if (++nodes_ > budget_)
        {
            aborted_ = true;
            return false;
        }
        return true;
    }

    void improve(uint64_t placed, const WindowState &w, int stalls)
    {
        if (best_ == 0 || aborted_)
        {
            return;
        }
        if (placed == full_)
        {
            if (stalls < best_)
            {
                best_ = stalls;
                bestOrder_ = path_;
            }
            return;
        }
        if (stalls >= best_ || deadEnd(placed) || !firstVisitAtCost(keyOf(placed, w), stalls))
        {
            return;
        }

        std::vector<Candidate> candidates;
        for (size_t i = 0; i < insts_.size(); ++i)
        {
            if (ready(i, placed))
            {
                WindowState after = w;
                const int cost = issue(after, insts_[i], kind_);
                if (stalls + cost < best_)
                {
                    candidates.push_back({i, cost, after});
                }
            }
        }
        // Same preference as the greedy scheduler, so good orders come first.
        std::sort(candidates.begin(), candidates.end(),
                  [this](const Candidate &a, const Candidate &b)
                  {
                      if (a.stalls != b.stalls) return a.stalls < b.stalls;
                      if (criticalPath_[a.index] != criticalPath_[b.index])
                          return criticalPath_[a.index] > criticalPath_[b.index];
                      if (insts_[a.index].is_load != insts_[b.index].is_load) return insts_[a.index].is_load;
                      return a.index < b.index;
                  });

        for (const Candidate &c : candidates)
        {
            if (stalls + c.stalls >= best_)
            {
                continue;
            }
            path_.push_back(c.index);
            improve(placed | (uint64_t{1} << c.index), c.after, stalls + c.stalls);
            path_.pop_back();
            if (best_ == 0 || aborted_)
            {
                return;
            }
        }
    }

    // Depth-first in original-index order: the first complete order with at
    // most `target` stalls, left in path_. Same dominance cut as improve().
    bool firstWithStalls(uint64_t placed, const WindowState &w, int stalls, int target)
    {
        if (placed == full_)
        {
            return stalls <= target;
        }
        if (aborted_ || deadEnd(placed) || !firstVisitAtCost(keyOf(placed, w), stalls))
        {
            return false;
        }
        for (size_t i = 0; i < insts_.size(); ++i)
        {
            if (!ready(i, placed))
            {
                continue;
            }
            WindowState after = w;
            const int cost = issue(after, insts_[i], kind_);
            if (stalls + cost > target)
            {
                continue;
            }
            path_.push_back(i);
            if (firstWithStalls(placed | (uint64_t{1} << i), after, stalls + cost, target))
            {
                return true;
            }
            path_.pop_back();
        }
        return false;
    }

    const std::vector<DecodedInst> &insts_;
    const std::vector<int> &criticalPath_;
    const PipelineKind kind_;
    const uint64_t budget_;
    const uint64_t full_;
    std::vector<uint64_t> predMask_;
    const PinnedSlots pins_;

    std::unordered_map<Key, int, KeyHash> seen_;
    std::vector<size_t> path_;
    std::vector<size_t> bestOrder_;
    int best_ = 0;
    uint64_t nodes_ = 0;
    bool aborted_ = false;
};

} // namespace Kites::reorder
