#pragma once
#include <cstdint>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/decoded_inst.h"

namespace Kites::reorder
{

// Which VM this prediction is for. Matches the hazard rule in rv5s_hdu.cpp:
// HF stalls only on load-use (1 cycle); HNF stalls on any true dependency
// with the instruction 1 back (2 cycles) or 2 back (1 cycle).
enum class PipelineKind : uint8_t { HF, HNF };

// Which source registers the hazard check compares against producers.
//  - Exact (default): what Kites' hazard unit does -- the rs1/rs2 bit fields
//    of EVERY instruction, even when those bits are part of an immediate
//    (the quirk). Matches the VM exactly on straight-line code.
//  - Textbook: only registers the instruction really reads (reads_rs1 /
//    reads_rs2), as a textbook pipeline would. For the quirk ablation only.
enum class HazardModel : uint8_t { Exact, Textbook };

// The two real instructions immediately before a block, used to seed the
// predictor's state at the block's head (so a hazard crossing into the block
// from outside can still be predicted).
//
// ASSUMPTION: no stall is still draining when the block starts, i.e. prev1
// and prev2 already finished decoding normally. This holds for the first
// block of a program, and for any block whose predecessor ends cleanly, but
// is not checked here -- a caller that reorders across blocks must make sure
// of it.
struct HeadContext
{
    const DecodedInst *prev1 = nullptr;   // instruction immediately before the block
    const DecodedInst *prev2 = nullptr;   // the one before that
};

// A GPR producer as the HDU sees it: does it really write a register (not
// x0), which one, and is it a load. Built from DecodedInst's RAW bit fields,
// because that is what rv5s_hdu.cpp actually reads -- see the quirk note on
// mustStayAfter() in dep_graph.h for why this differs from true reads/writes.
struct Producer
{
    bool writes = false;
    uint8_t rd = 0;
    bool isLoad = false;
};

inline Producer asProducer(const DecodedInst &x)
{
    Producer p;
    p.writes = x.writes_rd && x.rd != 0;
    p.rd = x.rd;
    p.isLoad = x.is_load;
    return p;
}

// Mirrors check_data_hazard() in rv5s_hdu.cpp exactly (GPR half only; this
// project is I/M scope, no F/D): `cur` is the instruction 1 cycle ahead of
// the consumer (what rv5s_hdu.cpp calls id_ex_reg / ex_mem_reg's current
// fields), `prev` is the one 2 cycles ahead (ex_mem_reg's prev_* fields).
inline int checkHazard(const Producer &cur, const Producer &prev,
                        uint8_t consumerRs1, uint8_t consumerRs2, PipelineKind kind)
{
    if (kind == PipelineKind::HF)
    {
        if (cur.writes && cur.isLoad && (cur.rd == consumerRs1 || cur.rd == consumerRs2))
        {
            return 1;
        }
        return 0;
    }

    if (cur.writes && (cur.rd == consumerRs1 || cur.rd == consumerRs2))
    {
        return 2;
    }
    if (prev.writes && (prev.rd == consumerRs1 || prev.rd == consumerRs2))
    {
        return 1;
    }
    return 0;
}

// Cycle-accurate simulation of JUST the HDU-visible front end: how many
// stall cycles the real VM would insert while decoding `block`, in this
// exact instruction order.
//
// This is NOT a per-pair "distance 1 or 2" lookup. rv5s_processor_h_f.cpp /
// rv5s_processor_h_nf.cpp run pipeline_execute() every cycle, stall or not,
// and pipeline_execute() shifts the producer window (current -> prev) using
// whatever is in id_ex_reg_ -- a NOP during a stall shifts the window exactly
// like a real instruction would. So a 2-cycle stall (HNF, distance-1 hazard)
// inserts 2 NOP-shifts and completely flushes BOTH window slots before the
// stalling instruction itself enters the window: every producer further back
// becomes invisible to every later instruction, not only to the one that
// stalled. A 1-cycle stall only flushes one slot. Confirmed against the real
// VM in reorder_cost_model_test.cpp (ReorderCostModel.Exp1_* and Exp3_HNF are
// the cases where a naive "nearest 2 real producers" model gets this wrong).
//
// issue() below reproduces that mechanism directly: a 2-slot producer
// window that shifts once per simulated cycle; a hazard inserts that many
// bubble cycles (each one shifting a bubble into the window, with no
// re-check while it drains), and then the same instruction is re-checked
// and charged again if it still hazards.
//
// The window between two instructions. After an instruction issues, these
// two slots are everything the hazard check will ever read again (the stall
// countdown is always 0 at that point), so the stalls charged to the rest of
// a block depend only on this window and the instructions still to come.
struct WindowState
{
    Producer older{};   // issued one cycle before `newer`, or a bubble if `newer` stalled first
    Producer newer{};   // the instruction issued last
};

inline WindowState windowFrom(const HeadContext &ctx)
{
    WindowState w;
    w.older = ctx.prev2 ? asProducer(*ctx.prev2) : Producer{};
    w.newer = ctx.prev1 ? asProducer(*ctx.prev1) : Producer{};
    return w;
}

// Issues one instruction from window `w`: returns the stall cycles charged
// to it and leaves `w` as the window right after it issued.
inline int issue(WindowState &w, const DecodedInst &consumer, PipelineKind kind,
                 HazardModel model = HazardModel::Exact)
{
    // Register 0 never matches a producer (asProducer only marks writes to rd != 0).
    const uint8_t rs1 = (model == HazardModel::Exact || consumer.reads_rs1) ? consumer.rs1 : 0;
    const uint8_t rs2 = (model == HazardModel::Exact || consumer.reads_rs2) ? consumer.rs2 : 0;
    // pipeline_execute() runs every cycle regardless of stall state, and
    // shifts the window using whatever is pending (a real instruction, or a
    // NOP left behind by a stall).
    Producer slotPrev = w.older;
    Producer slotCur = w.newer;
    int stalls = 0;
    while (true)
    {
        const int hazard = checkHazard(slotCur, slotPrev, rs1, rs2, kind);
        if (hazard == 0)
        {
            w.older = slotCur;
            w.newer = asProducer(consumer);
            return stalls;
        }
        stalls += hazard;
        for (int cycle = 0; cycle < hazard; ++cycle)   // id_ex_reg_.reset(): a bubble each cycle
        {
            slotPrev = slotCur;
            slotCur = Producer{};
        }
    }
}

// One way into a block: the hazard window it starts from, and how much its
// stalls count (an edge weight; see block_entries.h).
struct ScoredHead
{
    WindowState window{};
    int weight = 1;
};

// What an order of one block is scored on:
//   sum over heads h of  h.weight * (stalls of the block starting from h.window)
//                      + tailWeight * (stalls of `tail` issued right after it)
// `tail` is the first instructions of the block this one can fall through to
// (empty if it can't). One head of weight 1 and no tail is plain
// predictStalls() with that head's context.
struct BlockScoring
{
    std::vector<ScoredHead> heads;
    std::vector<DecodedInst> tail;
    int tailWeight = 0;
    HazardModel model = HazardModel::Exact;
};

inline BlockScoring singleContext(const HeadContext &ctx)
{
    BlockScoring s;
    s.heads.push_back(ScoredHead{windowFrom(ctx), 1});
    return s;
}

inline int scoreOrder(const std::vector<DecodedInst> &order, PipelineKind kind, const BlockScoring &scoring,
                      bool withTail = true)
{
    int total = 0;
    for (const ScoredHead &head : scoring.heads)
    {
        WindowState w = head.window;
        int stalls = 0;
        for (const DecodedInst &d : order)
        {
            stalls += issue(w, d, kind, scoring.model);
        }
        total += head.weight * stalls;
        if (withTail)
        {
            int tailStalls = 0;
            for (const DecodedInst &d : scoring.tail)
            {
                tailStalls += issue(w, d, kind, scoring.model);
            }
            total += scoring.tailWeight * tailStalls;
        }
    }
    return total;
}

inline int predictStalls(const std::vector<DecodedInst> &block, PipelineKind kind,
                          HeadContext ctx = {}, HazardModel model = HazardModel::Exact)
{
    WindowState w = windowFrom(ctx);
    int totalStalls = 0;
    for (const DecodedInst &consumer : block)
    {
        totalStalls += issue(w, consumer, kind, model);
    }
    return totalStalls;
}

// Convenience overload for the scheduler: decode once, predict on the
// resulting instructions.
inline int predictStalls(const std::vector<uint32_t> &words, PipelineKind kind,
                          HazardModel model = HazardModel::Exact)
{
    std::vector<DecodedInst> insts;
    insts.reserve(words.size());
    for (uint32_t w : words)
    {
        insts.push_back(decode(w));
    }
    return predictStalls(insts, kind, {}, model);
}

// Convenience overload for one block of a larger assembled program.
inline int predictStalls(const std::vector<uint32_t> &words, const Block &block,
                          PipelineKind kind, HeadContext ctx = {})
{
    std::vector<DecodedInst> insts;
    insts.reserve(block.end - block.begin);
    for (size_t k = block.begin; k < block.end; ++k)
    {
        insts.push_back(decode(words[k]));
    }
    return predictStalls(insts, kind, ctx);
}

} // namespace Kites::reorder
