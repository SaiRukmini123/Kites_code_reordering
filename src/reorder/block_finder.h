#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/assembled_program.h"
#include "reorder/decoded_inst.h"

namespace Kites::reorder
{

struct Block
{
    size_t begin;         // index of first instruction in the block
    size_t end;           // one past the last instruction (exclusive)
    bool hasTerminator;   // true if the last instruction is a branch or jump (it must stay put)
};

// Byte offset of a B-type (branch) instruction, sign-extended.
inline int32_t branchImm(uint32_t w)
{
    uint32_t imm = ((w >> 31) & 0x1) << 12 | ((w >> 7) & 0x1) << 11 |
                   ((w >> 25) & 0x3F) << 5 | ((w >> 8) & 0xF) << 1;
    return static_cast<int32_t>(imm << 19) >> 19;
}

// Byte offset of a J-type (jal) instruction, sign-extended.
inline int32_t jalImm(uint32_t w)
{
    uint32_t imm = ((w >> 31) & 0x1) << 20 | ((w >> 12) & 0xFF) << 12 |
                   ((w >> 20) & 0x1) << 11 | ((w >> 21) & 0x3FF) << 1;
    return static_cast<int32_t>(imm << 11) >> 11;
}

// extraLeaders: instruction indices that must start a new block even though
// nothing in `words` points at them. We need this because a jalr's target
// isn't in its bits (it comes from a register at run time), so a label that
// is only ever reached through a jalr is invisible to the scan below. The
// caller supplies those from the symbol table -- see labelLeaders() -- so
// the block boundary is still safe even though we can't compute it here.
inline std::vector<Block> findBlocks(const std::vector<uint32_t> &words,
                                     const std::vector<size_t> &extraLeaders = {})
{
    const size_t n = words.size();
    std::vector<Block> blocks;
    if (n == 0)
        return blocks;

    // leader[i] == true means "a new block starts at instruction i".
    // Index n is a sentinel that closes the last block.
    std::vector<bool> leader(n + 1, false);
    leader[0] = true;
    leader[n] = true;

    for (size_t idx : extraLeaders)
        if (idx < n)
            leader[idx] = true;

    for (size_t i = 0; i < n; ++i)
    {
        const DecodedInst d = decode(words[i]);

        if (d.is_branch || d.is_jump)
        {
            leader[i + 1] = true;                       // fall-through / return point

            if (d.kind == Kind::Branch || d.kind == Kind::Jal)
            {
                const int32_t off = (d.kind == Kind::Branch) ? branchImm(words[i])
                                                             : jalImm(words[i]);
                if (off % 4 == 0)
                {
                    const int64_t target = static_cast<int64_t>(i) + off / 4;
                    if (target >= 0 && target < static_cast<int64_t>(n))
                        leader[static_cast<size_t>(target)] = true;
                }
            }
        }
        else if (d.is_barrier)                          // ecall, auipc, unknown
        {
            leader[i] = true;                           // barrier starts its own block
            leader[i + 1] = true;                       // and ends it
        }
    }

    size_t begin = 0;
    for (size_t i = 1; i <= n; ++i)
    {
        if (!leader[i])
            continue;
        const DecodedInst last = decode(words[i - 1]);
        blocks.push_back({begin, i, last.is_branch || last.is_jump});
        begin = i;
    }
    return blocks;
}

// Every code label the assembler recorded, converted to an instruction index,
// so it can be passed as findBlocks()'s extraLeaders. A jalr can jump to any
// label (the assembler doesn't know which one, and neither do we), so the
// only safe choice is to treat every label as a possible jalr target.
// Data labels (isData) don't bound code and are skipped.
inline std::vector<size_t> labelLeaders(const Kites::AssembledProgram &program)
{
    std::vector<size_t> leaders;
    for (const auto &[name, sym] : program.symbol_table)
    {
        (void)name;
        if (sym.isData || sym.address % 4 != 0)
            continue;
        leaders.push_back(static_cast<size_t>(sym.address / 4));
    }
    return leaders;
}

} // namespace Kites::reorder
