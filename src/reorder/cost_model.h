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
// The loop below reproduces that mechanism directly: a 2-slot producer
// window that shifts once per simulated cycle, and a stall counter that is
// decremented without re-checking while it drains, then re-checked (and
// charged again if it still hazards) once it reaches zero.
inline int predictStalls(const std::vector<DecodedInst> &block, PipelineKind kind,
                          HeadContext ctx = {})
{
    Producer slotCur = ctx.prev2 ? asProducer(*ctx.prev2) : Producer{};
    Producer pending = ctx.prev1 ? asProducer(*ctx.prev1) : Producer{};
    Producer slotPrev{};

    int stallCyclesLeft = 0;
    int totalStalls = 0;
    size_t idx = 0;

    while (idx < block.size())
    {
        // pipeline_execute() runs this cycle regardless of stall state, and
        // shifts the window using whatever is pending (a real instruction,
        // or a NOP left behind by a previous stall).
        slotPrev = slotCur;
        slotCur = pending;

        if (stallCyclesLeft > 0)
        {
            --stallCyclesLeft;
            ++totalStalls;
            pending = Producer{};   // id_ex_reg_.reset(): still a bubble next cycle
            continue;               // same consumer, not re-checked until the drain ends
        }

        const DecodedInst &consumer = block[idx];
        int stalls = checkHazard(slotCur, slotPrev, consumer.rs1, consumer.rs2, kind);
        if (stalls > 0)
        {
            ++totalStalls;
            stallCyclesLeft = stalls - 1;
            pending = Producer{};
        }
        else
        {
            pending = asProducer(consumer);
            ++idx;
        }
    }
    return totalStalls;
}

// Convenience overload for the scheduler: decode once, predict on the
// resulting instructions.
inline int predictStalls(const std::vector<uint32_t> &words, PipelineKind kind)
{
    std::vector<DecodedInst> insts;
    insts.reserve(words.size());
    for (uint32_t w : words)
    {
        insts.push_back(decode(w));
    }
    return predictStalls(insts, kind);
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
