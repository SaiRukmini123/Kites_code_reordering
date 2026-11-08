#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "common/assembled_program.h"
#include "common/instructions.h"
#include "reorder/block_finder.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/scheduler.h"

namespace Kites::reorder
{

inline std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// The code part of a source line: comment (# or ;) and outer whitespace removed.
inline std::string stripComment(const std::string &line)
{
    std::string code = line.substr(0, line.find_first_of("#;"));
    const size_t first = code.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return "";
    }
    const size_t last = code.find_last_not_of(" \t");
    return code.substr(first, last - first + 1);
}

// Every line of every non-text section (.data, .bss, or .section other than
// .text), copied verbatim, in source order. Lines before any directive are
// text, matching the Kites assembler's default.
inline std::string extractDataSections(const std::string &source)
{
    std::ostringstream out;
    bool inData = false;
    for (const std::string &line : splitLines(source))
    {
        std::istringstream tokens(stripComment(line));
        std::string first, second;
        tokens >> first >> second;
        if (first == ".data" || first == ".bss")
        {
            inData = true;
        }
        else if (first == ".text")
        {
            inData = false;
        }
        else if (first == ".section")
        {
            inData = (second != ".text");
        }

        if (inData)
        {
            out << line << "\n";
        }
    }
    return out.str();
}

// Kites::instruction_set::disassemble(), except for the two cases where its
// text does not re-assemble to the same word (checked against every example):
//  - srai with shamt >= 32: bit 25 is shamt[5], so funct7 reads 0b0100001 and
//    disassemble() prints "srli";
//  - fcvt.{s,d}.{w,wu,l,lu}: printed with the format twice ("fcvt.s.w.s"),
//    which the assembler rejects.
inline std::string disassembleForKites(uint32_t w)
{
    const uint32_t opcode = w & 0x7F;
    const uint32_t funct3 = (w >> 12) & 0x7;
    const uint32_t funct7 = w >> 25;
    const uint32_t rd = (w >> 7) & 0x1F, rs1 = (w >> 15) & 0x1F, rs2 = (w >> 20) & 0x1F;
    auto xr = [](uint32_t r) { return "x" + std::to_string(r); };
    auto fr = [](uint32_t r) { return "f" + std::to_string(r); };

    if (opcode == 0x13 && funct3 == 5 && ((w >> 30) & 1) != 0)
    {
        return "srai " + xr(rd) + ", " + xr(rs1) + ", " + std::to_string((w >> 20) & 0x3F);
    }
    if (opcode == 0x53 && (funct7 & ~1u) == 0b1101000 && rs2 < 4)
    {
        static const char *source[] = {".w", ".wu", ".l", ".lu"};
        return std::string("fcvt") + ((funct7 & 1) != 0 ? ".d" : ".s") + source[rs2] + " " + fr(rd) +
               ", " + xr(rs1);
    }
    return Kites::instruction_set::disassemble(w);
}

// For a branch or jal at instruction position `pos`, the instruction index it
// jumps to. jalr is not direct (its target comes from a register at run time).
inline bool directTarget(uint32_t word, size_t pos, int64_t &target)
{
    const DecodedInst d = decode(word);
    if (d.kind != Kind::Branch && d.kind != Kind::Jal)
    {
        return false;
    }
    const int32_t off = (d.kind == Kind::Branch) ? branchImm(word) : jalImm(word);
    if (off % 4 != 0)
    {
        return false;
    }
    target = static_cast<int64_t>(pos) + off / 4;
    return true;
}

// "L", or "L_", "L__"... if an existing label already looks like L<digits>,
// so a generated L<index> can never collide with a label from the source.
inline std::string generatedLabelPrefix(const Kites::AssembledProgram &program)
{
    std::string prefix = "L";
    auto clashes = [&program](const std::string &p)
    {
        for (const auto &[name, sym] : program.symbol_table)
        {
            (void)sym;
            if (name.size() > p.size() && name.compare(0, p.size(), p) == 0 &&
                name.find_first_not_of("0123456789", p.size()) == std::string::npos)
            {
                return true;
            }
        }
        return false;
    };
    while (clashes(prefix))
    {
        prefix += "_";
    }
    return prefix;
}

// Original code labels by instruction index (index n = just past the end).
inline std::map<size_t, std::vector<std::string>> textLabelsByIndex(const Kites::AssembledProgram &program)
{
    std::map<size_t, std::vector<std::string>> labels;
    const size_t n = program.text_buffer.size();
    for (const auto &[name, sym] : program.symbol_table)
    {
        if (!sym.isData && sym.address % 4 == 0 && sym.address / 4 <= n)
        {
            labels[static_cast<size_t>(sym.address / 4)].push_back(name);
        }
    }
    return labels;
}

// For every position in report.words, the instruction index it had in the
// original program.
inline std::vector<size_t> originalIndexOf(const ScheduleReport &report)
{
    std::vector<size_t> original(report.words.size());
    std::iota(original.begin(), original.end(), 0);
    for (const Move &m : report.moves)
    {
        original[m.newIndex] = m.originalIndex;
    }
    return original;
}

// 1-based source line of the instruction at original index `index`, or 0.
inline unsigned sourceLineOf(const Kites::AssembledProgram &program, size_t index)
{
    auto it = program.instruction_number_line_number_mapping.find(static_cast<unsigned>(index));
    return it == program.instruction_number_line_number_mapping.end() ? 0 : it->second;
}

inline std::string sourceTextOf(const std::vector<std::string> &lines, unsigned lineNumber)
{
    if (lineNumber == 0 || lineNumber > lines.size())
    {
        return "?";
    }
    return stripComment(lines[lineNumber - 1]);
}

// The scheduled program as Kites assembly: the original data sections
// verbatim, then .text with one line per word of report.words. Every
// original code label is written at its index, and branch/jal targets use
// that label's name; a generated L<index> is added only for a target with no
// label in the source. Scheduling never moves an instruction across a block
// boundary, so every target index and every branch/jal position is
// unchanged and the labels resolve to the same offsets as the original.
inline std::string writeAssembly(const std::string &source, const Kites::AssembledProgram &program,
                                 const ScheduleReport &report)
{
    const std::vector<uint32_t> &words = report.words;
    const size_t n = words.size();
    const std::vector<std::string> lines = splitLines(source);
    const std::vector<size_t> original = originalIndexOf(report);
    const std::string prefix = generatedLabelPrefix(program);

    std::map<size_t, std::vector<std::string>> labels = textLabelsByIndex(program);
    for (size_t pos = 0; pos < n; ++pos)
    {
        int64_t t = 0;
        if (directTarget(words[pos], pos, t) && t >= 0 && t <= static_cast<int64_t>(n) &&
            labels.count(static_cast<size_t>(t)) == 0)
        {
            labels[static_cast<size_t>(t)].push_back(prefix + std::to_string(t));
        }
    }

    std::ostringstream out;
    const std::string data = extractDataSections(source);
    if (!data.empty())
    {
        out << data << "\n";
    }
    out << ".text\n";

    auto writeLabels = [&out, &labels](size_t index)
    {
        auto it = labels.find(index);
        if (it != labels.end())
        {
            for (const std::string &name : it->second)
            {
                out << name << ":\n";
            }
        }
    };

    for (size_t pos = 0; pos < n; ++pos)
    {
        writeLabels(pos);

        std::string text = disassembleForKites(words[pos]);
        int64_t t = 0;
        if (directTarget(words[pos], pos, t) && t >= 0 && labels.count(static_cast<size_t>(t)) != 0)
        {
            // Both "beq rs1, rs2, imm" and "jal rd, imm" end with ", imm".
            text = text.substr(0, text.rfind(", ") + 2) + labels[static_cast<size_t>(t)].front();
        }

        const unsigned line = sourceLineOf(program, original[pos]);
        out << "    " << std::left << std::setw(28) << text << "# line " << line << ": "
            << sourceTextOf(lines, line);
        if (original[pos] != pos)
        {
            out << " (moved from " << original[pos] << ")";
        }
        out << "\n";
    }
    writeLabels(n);
    return out.str();
}

// keep[k] is true for the positions of ONE longest strictly increasing
// subsequence of seq (values must be distinct). O(n log n) patience method.
inline std::vector<bool> longestIncreasingSubsequence(const std::vector<size_t> &seq)
{
    const size_t none = static_cast<size_t>(-1);
    std::vector<size_t> tails;               // tails[k]: position ending the best run of length k+1
    std::vector<size_t> previous(seq.size(), none);
    for (size_t i = 0; i < seq.size(); ++i)
    {
        auto it = std::lower_bound(tails.begin(), tails.end(), seq[i],
                                   [&seq](size_t pos, size_t value) { return seq[pos] < value; });
        if (it != tails.begin())
        {
            previous[i] = *(it - 1);
        }
        if (it == tails.end())
        {
            tails.push_back(i);
        }
        else
        {
            *it = i;
        }
    }

    std::vector<bool> keep(seq.size(), false);
    for (size_t i = tails.empty() ? none : tails.back(); i != none; i = previous[i])
    {
        keep[i] = true;
    }
    return keep;
}

// One suggested edit to the original source: move one instruction above
// (hoist) or below (sink) an anchor instruction that stays where it is.
struct SuggestedMove
{
    size_t originalIndex = 0;
    size_t anchorOriginalIndex = 0;
    bool hoist = true;          // true: now above the anchor it originally followed
    size_t blockIndex = 0;      // index into report.blocks
};

// The minimal set of moves: per block, every instruction that is NOT in a
// longest increasing subsequence of original indices (in the new order).
// Everything in that subsequence keeps its relative order, so only the rest
// needs to move. The anchor is the nearest kept instruction: the next one in
// the new order if the moved instruction originally came after it (hoist),
// otherwise the previous one, which it then originally came before (sink).
// One of the two always holds, or the subsequence could be made longer.
inline std::vector<SuggestedMove> suggestedMoves(const ScheduleReport &report)
{
    const std::vector<size_t> original = originalIndexOf(report);
    std::vector<SuggestedMove> moves;
    for (size_t b = 0; b < report.blocks.size(); ++b)
    {
        const Block &block = report.blocks[b].block;
        const std::vector<size_t> seq(original.begin() + static_cast<std::ptrdiff_t>(block.begin),
                                      original.begin() + static_cast<std::ptrdiff_t>(block.end));
        const std::vector<bool> keep = longestIncreasingSubsequence(seq);

        for (size_t k = 0; k < seq.size(); ++k)
        {
            if (keep[k])
            {
                continue;
            }
            SuggestedMove m;
            m.originalIndex = seq[k];
            m.blockIndex = b;

            size_t next = k + 1;
            while (next < seq.size() && !keep[next])
            {
                ++next;
            }
            if (next < seq.size() && seq[k] > seq[next])
            {
                m.hoist = true;
                m.anchorOriginalIndex = seq[next];
            }
            else
            {
                size_t prev = k;
                while (prev > 0 && !keep[prev - 1])
                {
                    --prev;
                }
                m.hoist = false;
                m.anchorOriginalIndex = seq[prev - 1];
            }
            moves.push_back(m);
        }
    }
    return moves;
}

// Human-readable report: the minimal suggested moves per block, predicted
// stalls per block, then every position change (old -> new) for reference.
inline std::string writeSuggestions(const std::string &source, const Kites::AssembledProgram &program,
                                    const ScheduleReport &report, PipelineKind kind)
{
    const std::vector<std::string> lines = splitLines(source);
    auto describe = [&](size_t originalIndex)
    {
        const unsigned line = sourceLineOf(program, originalIndex);
        return "line " + std::to_string(line) + " " + sourceTextOf(lines, line);
    };
    auto lineRange = [&](const Block &block)
    {
        return "lines " + std::to_string(sourceLineOf(program, block.begin)) + "-" +
               std::to_string(sourceLineOf(program, block.end - 1));
    };

    std::ostringstream out;
    out << "Reorder suggestions for " << (kind == PipelineKind::HF ? "rv5-h-f" : "rv5-h-nf")
        << "\nPredicted stalls: " << report.stallsBefore << " -> " << report.stallsAfter << "\n\n";

    const std::vector<SuggestedMove> moves = suggestedMoves(report);
    out << "Suggested moves (" << moves.size()
        << "; every other instruction keeps its order):\n";
    if (moves.empty())
    {
        out << "  none\n";
    }
    size_t shownBlock = static_cast<size_t>(-1);
    for (const SuggestedMove &m : moves)
    {
        if (m.blockIndex != shownBlock)
        {
            shownBlock = m.blockIndex;
            const BlockStalls &b = report.blocks[m.blockIndex];
            out << "  block " << lineRange(b.block) << " (predicted stalls " << b.stallsBefore
                << " -> " << b.stallsAfter << "):\n";
        }
        out << "    " << (m.hoist ? "hoist " : "sink ") << describe(m.originalIndex)
            << (m.hoist ? " above " : " below ") << describe(m.anchorOriginalIndex) << "\n";
    }

    out << "\nPer block (predicted stalls before -> after):\n";
    for (const BlockStalls &b : report.blocks)
    {
        out << "  instructions " << b.block.begin << "-" << (b.block.end - 1) << " ("
            << lineRange(b.block) << "): " << b.stallsBefore << " -> " << b.stallsAfter << "\n";
    }

    std::vector<Move> positions = report.moves;
    std::sort(positions.begin(), positions.end(),
              [](const Move &a, const Move &b) { return a.newIndex < b.newIndex; });
    out << "\nAll position changes, old -> new (" << positions.size() << "):\n";
    if (positions.empty())
    {
        out << "  none\n";
    }
    for (const Move &m : positions)
    {
        const unsigned line = sourceLineOf(program, m.originalIndex);
        out << "  line " << std::left << std::setw(4) << line << std::setw(26)
            << sourceTextOf(lines, line) << "position " << m.originalIndex << " -> " << m.newIndex
            << "\n";
    }
    return out.str();
}

} // namespace Kites::reorder
