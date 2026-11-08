#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "reorder/block_finder.h"
#include "reorder/decoded_inst.h"

namespace Kites::reorder
{

// The dependency graph of ONE basic block.
//
// Node k stands for instruction (block.begin + k), so node 0 is the first
// instruction of the block. An edge i -> j (with i < j) means:
// "instruction j must stay after instruction i, or the program changes meaning".
// Any order of the nodes that respects every edge is a safe reordering.
struct DepGraph
{
    size_t size = 0;                           // number of instructions in the block
    std::vector<std::vector<size_t>> succ;     // succ[i] = nodes that must come after i
    std::vector<std::vector<size_t>> pred;     // pred[j] = nodes that must come before j

    bool hasEdge(size_t i, size_t j) const
    {
        for (size_t s : succ[i])
            if (s == j)
                return true;
        return false;
    }
};

// Does instruction x really write a register? Writing x0 does nothing in RISC-V,
// so it never counts.
inline bool writesReg(const DecodedInst &x)
{
    return x.writes_rd && x.rd != 0;
}

// Must `later` stay after `earlier`? (earlier comes first in the original program)
// This uses only TRUE reads (reads_rs1 / reads_rs2), never the raw bit fields,
// because a field that is really part of an immediate does not read any register.
inline bool mustStayAfter(const DecodedInst &earlier, const DecodedInst &later)
{
    // Barriers (branch, jump, ecall, auipc, unknown) are ordered against everything.
    if (earlier.is_barrier || later.is_barrier)
        return true;

    // RAW: later reads a register that earlier writes.
    const bool raw = writesReg(earlier) &&
                     ((later.reads_rs1 && later.rs1 == earlier.rd) ||
                      (later.reads_rs2 && later.rs2 == earlier.rd));

    // WAR: later writes a register that earlier reads.
    const bool war = writesReg(later) &&
                     ((earlier.reads_rs1 && earlier.rs1 == later.rd) ||
                      (earlier.reads_rs2 && earlier.rs2 == later.rd));

    // WAW: both write the same register.
    const bool waw = writesReg(earlier) && writesReg(later) && earlier.rd == later.rd;

    // Memory: we cannot tell if two addresses are equal, so assume they might be.
    // Two loads never conflict; every other pair of memory operations does.
    const bool earlierMem = earlier.is_load || earlier.is_store;
    const bool laterMem = later.is_load || later.is_store;
    const bool mem = earlierMem && laterMem && (earlier.is_store || later.is_store);

    return raw || war || waw || mem;
}

// True when mustStayAfter(earlier, later) is forced only by the conservative
// memory clause -- no barrier and no true register dependency (RAW/WAR/WAW)
// links them. Mirrors mustStayAfter's own sub-conditions so callers that
// already know mustStayAfter is true can tell whether memory is the sole
// reason, without re-deriving it differently.
inline bool linkedOnlyByMemory(const DecodedInst &earlier, const DecodedInst &later)
{
    if (earlier.is_barrier || later.is_barrier)
        return false;

    const bool raw = writesReg(earlier) &&
                     ((later.reads_rs1 && later.rs1 == earlier.rd) ||
                      (later.reads_rs2 && later.rs2 == earlier.rd));
    const bool war = writesReg(later) &&
                     ((earlier.reads_rs1 && earlier.rs1 == later.rd) ||
                      (earlier.reads_rs2 && earlier.rs2 == later.rd));
    const bool waw = writesReg(earlier) && writesReg(later) && earlier.rd == later.rd;

    return !raw && !war && !waw;
}

// Can the conservative memory edge between insts[i] and insts[j] (i < j,
// both loads/stores, at least one a store) be dropped?
//
// Safe only when all of these hold:
//  - both use the same base register (the raw rs1 bit field -- where both
//    I-type loads and S-type stores always put their base);
//  - that register is written by neither op itself (e.g. lw x5,0(x5), which
//    would make the "same register" mean a different value depending on
//    which of the two runs first) nor by anything strictly between them;
//  - their byte ranges [offset, offset+size) don't overlap.
//
// Why this is safe to reorder: dropping this edge only lets the scheduler
// swap these two memory ops relative to EACH OTHER. Any instruction that
// writes the shared base register from outside this window is already
// ordered against both i and j by an ordinary RAW/WAR edge on that same
// register (ordinary dependency tracking, untouched by this function), so
// it keeps producing the same base value before whichever of the two runs
// first. Since nothing inside the window changes that value either, both
// ops see the identical base value regardless of their relative order, so
// their effective addresses -- and hence whether they truly conflict --
// never depend on how the scheduler orders them.
inline bool canDropMemoryEdge(const std::vector<DecodedInst> &insts, size_t i, size_t j)
{
    const DecodedInst &a = insts[i];
    const DecodedInst &b = insts[j];
    if (a.rs1 != b.rs1)
        return false;

    const uint8_t baseReg = a.rs1;
    for (size_t k = i; k <= j; ++k)
        if (writesReg(insts[k]) && insts[k].rd == baseReg)
            return false;

    const int64_t aStart = a.mem_offset, aEnd = aStart + a.mem_size;
    const int64_t bStart = b.mem_offset, bEnd = bStart + b.mem_size;
    return aEnd <= bStart || bEnd <= aStart;
}

// memDisambiguation = false restores the fully conservative memory rule
// (every pair with a store keeps its order), for A/B comparison.
inline DepGraph buildDepGraph(const std::vector<uint32_t> &words, const Block &block,
                              bool memDisambiguation = true)
{
    DepGraph g;
    g.size = block.end - block.begin;
    g.succ.assign(g.size, {});
    g.pred.assign(g.size, {});

    std::vector<DecodedInst> insts;
    insts.reserve(g.size);
    for (size_t k = block.begin; k < block.end; ++k)
        insts.push_back(decode(words[k]));

    for (size_t i = 0; i < g.size; ++i)
        for (size_t j = i + 1; j < g.size; ++j)
        {
            if (!mustStayAfter(insts[i], insts[j]))
                continue;

            if (memDisambiguation && linkedOnlyByMemory(insts[i], insts[j]) &&
                canDropMemoryEdge(insts, i, j))
                continue;

            g.succ[i].push_back(j);
            g.pred[j].push_back(i);
        }
    return g;
}

} // namespace Kites::reorder
