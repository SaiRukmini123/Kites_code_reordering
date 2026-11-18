#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

#include "reorder/decoded_inst.h"
#include "reorder/dep_graph.h"

namespace Kites::reorder
{

// Pinned instructions (auipc) of one block must stay at their own slot:
// local instruction i, if pinned, is placed at position i. Its successors
// can't come earlier anyway (they are not ready until it is placed), so the
// only way a schedule can fail is by running out of slots before a pin for
// the instructions that must precede it. This tracks that.
struct PinnedSlots
{
    bool any = false;
    std::vector<bool> pinned;       // pinned[i]: instruction i is pinned (and slot i is its slot)
    std::vector<size_t> deadline;   // last slot instruction i may take; slot i itself if pinned

    PinnedSlots(const std::vector<DecodedInst> &insts, const DepGraph &g)
        : pinned(insts.size(), false), deadline(insts.size(), 0)
    {
        const size_t n = insts.size();
        for (size_t i = 0; i < n; ++i)
        {
            pinned[i] = insts[i].is_pinned;
            any = any || pinned[i];
        }
        // Edges only go from a lower to a higher index, so walking down
        // visits every successor first. An instruction must be placed one
        // slot before each successor's deadline, so a chain of k steps to a
        // pin at slot p must start by slot p - k. The original order
        // satisfies all of these, so deadline[i] >= i.
        for (size_t i = n; i-- > 0;)
        {
            if (pinned[i])
            {
                deadline[i] = i;
                continue;
            }
            size_t d = n - 1;
            for (size_t j : g.succ[i])
            {
                d = std::min(d, deadline[j] - 1);
            }
            deadline[i] = d;
        }
    }

    // Can the instructions not yet placed fill slots nextSlot..n-1 so that
    // every pinned one lands on its slot and every edge is respected?
    //
    // Earliest-deadline-first: at a pinned slot place that instruction (it
    // must be ready); at any other slot place the ready unpinned instruction
    // with the earliest deadline, which must not have passed. For unit-length
    // jobs on one machine with precedence constraints, once deadlines are
    // tightened along the edges as above, this finds an order whenever one
    // exists (pinned instructions are jobs whose earliest and latest slot
    // coincide). reorder_pinning_test checks it against brute force.
    bool canComplete(const std::vector<bool> &placed, size_t nextSlot, const DepGraph &g) const
    {
        const size_t n = pinned.size();
        std::vector<bool> done = placed;
        std::vector<size_t> waiting(n, 0);   // unplaced predecessors
        for (size_t j = 0; j < n; ++j)
        {
            for (size_t i : g.pred[j])
            {
                waiting[j] += done[i] ? 0 : 1;
            }
        }
        for (size_t slot = nextSlot; slot < n; ++slot)
        {
            size_t pick = n;
            if (pinned[slot])
            {
                if (done[slot] || waiting[slot] != 0)
                {
                    return false;
                }
                pick = slot;
            }
            else
            {
                for (size_t i = 0; i < n; ++i)
                {
                    if (!done[i] && !pinned[i] && waiting[i] == 0 && (pick == n || deadline[i] < deadline[pick]))
                    {
                        pick = i;
                    }
                }
                if (pick == n || deadline[pick] < slot)
                {
                    return false;
                }
            }
            done[pick] = true;
            for (size_t s : g.succ[pick])
            {
                --waiting[s];
            }
        }
        return true;
    }

    // Cheap necessary condition for the exhaustive search: is any unplaced
    // unpinned instruction already past its deadline at nextSlot?
    template <typename IsPlaced>
    bool someDeadlinePassed(size_t nextSlot, IsPlaced isPlaced) const
    {
        for (size_t i = 0; i < pinned.size(); ++i)
        {
            if (!pinned[i] && deadline[i] < nextSlot && !isPlaced(i))
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace Kites::reorder
