#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "assembler/assembler.h"
#include "reorder/asm_writer.h"
#include "reorder/bench_eval.h"
#include "reorder/block_entries.h"
#include "reorder/block_finder.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/reorder_report.h"
#include "reorder/scheduler.h"
#include "reorder/unroll_predict.h"
#include "reorder/vm_safety_net.h"

namespace Kites::reorder
{

// Loop unrolling, part 2 of 2 (transform and verify). Opt-in: nothing in the
// existing pipeline calls it, and it changes no existing behaviour.
//
// unrollAndReorder() runs predict -> transform -> verify for one program on
// one pipeline:
//  1. Reorder-only, the existing pipeline: Optimal, then the VM safety net.
//  2. Predict (unroll_predict.h): for every eligible loop, the saving at k = 2
//     and k = 4 with the reorder-only order in every copy. The larger saving
//     wins (the smaller k on a tie); loops with no predicted gain are skipped.
//  3. Transform the .s text (unrollSource()): copy the loop's body k - 1 more
//     times, put the inverted branch to a new exit label after every copy but
//     the last, keep the original back-branch after the last copy, and put the
//     exit label right after it.
//  4. Verify each loop on its own: re-assemble, check the copies word for word
//     (unrolledShapeMatches()), reorder (Optimal + safety net) and run. A loop
//     passes when the final registers and data window (sameState) and the
//     retired count equal the original's, and it is faster than reorder-only.
//  5. Combine the loops that passed and check the combined program the same
//     way. If it fails, keep the best single loop that passed; fall back to
//     reorder-only when none did.

// The branch with the opposite condition and the same operands, or "" if
// `mnemonic` is not a conditional branch. Covers the base branches and the
// Kites pseudo-branches.
inline std::string invertedBranch(const std::string &mnemonic)
{
    static const std::pair<const char *, const char *> pairs[] = {
        {"beq", "bne"},   {"blt", "bge"},   {"bltu", "bgeu"}, {"bgt", "ble"},
        {"bgtu", "bleu"}, {"beqz", "bnez"}, {"bltz", "bgez"}, {"bgtz", "blez"},
    };
    for (const auto &[a, b] : pairs)
    {
        if (mnemonic == a)
        {
            return b;
        }
        if (mnemonic == b)
        {
            return a;
        }
    }
    return "";
}

// Operands a conditional branch is written with, the target included: 2 for
// the compare-with-zero forms (beqz ... blez), 3 for the rest, 0 otherwise.
inline int branchOperandCount(const std::string &mnemonic)
{
    if (invertedBranch(mnemonic).empty())
    {
        return 0;
    }
    return mnemonic.back() == 'z' ? 2 : 3;
}

namespace unroll_detail
{
inline std::string trim(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return "";
    }
    const size_t last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

inline bool isLabelName(const std::string &s)
{
    if (s.empty())
    {
        return false;
    }
    for (char c : s)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '$')
        {
            return false;
        }
    }
    return true;
}

inline bool isNumber(const std::string &s)
{
    const size_t digit = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
    return digit < s.size() && std::isdigit(static_cast<unsigned char>(s[digit]));
}

// One source line as the Kites assembler reads it: leading "name:" labels, the
// instruction (mnemonic and comma-separated operands), and a comment from # or
// ; to the end of the line.
struct SourceLine
{
    std::vector<std::string> labels;
    std::string code;                    // the instruction, without labels and comment
    std::string mnemonic;                // lower case
    std::vector<std::string> operands;
    std::string comment;                 // starting at the # or ;
};

inline SourceLine parseLine(const std::string &line)
{
    SourceLine p;
    const size_t mark = line.find_first_of("#;");
    std::string code = trim(mark == std::string::npos ? line : line.substr(0, mark));
    if (mark != std::string::npos)
    {
        p.comment = line.substr(mark);
    }
    for (size_t colon = code.find(':'); colon != std::string::npos; colon = code.find(':'))
    {
        const std::string name = trim(code.substr(0, colon));
        if (!isLabelName(name))
        {
            break;
        }
        p.labels.push_back(name);
        code = trim(code.substr(colon + 1));
    }
    p.code = code;
    const size_t space = code.find_first_of(" \t");
    p.mnemonic = code.substr(0, space);
    for (char &c : p.mnemonic)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (space != std::string::npos)
    {
        const std::string rest = code.substr(space);
        size_t start = 0;
        while (true)
        {
            const size_t comma = rest.find(',', start);
            const std::string operand =
                trim(rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!operand.empty())
            {
                p.operands.push_back(operand);
            }
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }
    return p;
}

// For every line: true if the assembler reads it as code (the section rules of
// extractDataSections() in asm_writer.h; lines before any directive are code).
inline std::vector<bool> codeLines(const std::vector<std::string> &lines)
{
    std::vector<bool> code(lines.size(), true);
    bool inData = false;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::istringstream tokens(stripComment(lines[i]));
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
        code[i] = !inData;
    }
    return code;
}
} // namespace unroll_detail

// One loop to unroll: an eligible loop of the program (unroll_predict.h) and
// the number of copies.
struct LoopToUnroll
{
    Block block{};
    int k = 2;
};

struct UnrolledSource
{
    bool ok = false;
    std::string text;                      // the new source, when ok
    std::string reason;                    // why not, when !ok
    std::vector<std::string> exitLabels;   // one per loop, in the order given
};

// The name of the label after an unrolled loop: unroll_exit_L<line of the
// back-branch>, with '_' appended until it clashes with no existing name.
// Only letters, digits and '_', and never a section name such as "data",
// which hangs the Kites assembler.
inline std::string exitLabelName(unsigned branchLine, std::set<std::string> &taken)
{
    std::string name = "unroll_exit_L" + std::to_string(branchLine);
    while (taken.count(name) != 0)
    {
        name += "_";
    }
    taken.insert(name);
    return name;
}

// Rewrites `source` (the text `program` was assembled from) with every loop in
// `loops` unrolled k times:
//     <loop's labels and copy 1, as written>
//         <inverted branch> <operands>, unroll_exit_L<n>   (after copies 1 .. k-1)
//     <copies 2 .. k: the same lines, without the labels on the first line>
//     <the original back-branch line, after copy k>
//     unroll_exit_L<n>:
// The loop's lines come from the assembler's instruction-to-line map, so
// comments and pseudo-instructions are copied as written. Fails, with the
// reason, when unrolling cannot be done safely on the text: the loop's lines
// also hold other code, the back-branch is not a branch to the loop's own
// label, the body has a label or a directive, or the program has a hand-written
// auipc or a branch/jump to a numeric offset (unrolling moves code, and only
// labels follow it).
inline UnrolledSource unrollSource(const std::string &source, const AssembledProgram &program,
                                   const std::vector<LoopToUnroll> &loops)
{
    using unroll_detail::SourceLine;
    UnrolledSource out;
    auto fail = [&out](const std::string &why)
    {
        out.ok = false;
        out.reason = why;
        out.text.clear();
        out.exitLabels.clear();
        return out;
    };

    std::vector<std::string> lines = splitLines(source);
    const std::vector<bool> isCode = unroll_detail::codeLines(lines);
    const std::vector<uint32_t> &words = program.text_buffer;

    std::set<std::string> names;
    for (const auto &[name, sym] : program.symbol_table)
    {
        (void)sym;
        names.insert(name);
    }
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const SourceLine p = unroll_detail::parseLine(lines[i]);
        names.insert(p.labels.begin(), p.labels.end());
        if (!isCode[i] || p.mnemonic.empty())
        {
            continue;
        }
        const std::string where = "line " + std::to_string(i + 1);
        if (p.mnemonic == "auipc")
        {
            return fail(where + " has a hand-written auipc, whose offset would no longer match");
        }
        const bool toTarget = branchOperandCount(p.mnemonic) != 0 || p.mnemonic == "j" || p.mnemonic == "jal" ||
                              p.mnemonic == "call" || p.mnemonic == "tail";
        if (toTarget && !p.operands.empty() && unroll_detail::isNumber(p.operands.back()))
        {
            return fail(where + " branches to a numeric offset, which would no longer match");
        }
    }

    struct Region
    {
        unsigned firstLine = 0;    // line of the loop's first instruction
        unsigned branchLine = 0;   // line of its back-branch
        int k = 2;
        SourceLine branch;
        std::string exitLabel;
    };
    std::vector<Region> regions;
    for (const LoopToUnroll &loop : loops)
    {
        const Block &b = loop.block;
        if (!isEligibleLoop(words, b))
        {
            return fail("instructions " + std::to_string(b.begin) + "-" + std::to_string(b.end - 1) +
                        " are not an eligible loop");
        }
        if (loop.k < 2)
        {
            return fail("k must be 2 or more");
        }
        Region r;
        r.k = loop.k;
        r.firstLine = sourceLineOf(program, b.begin);
        r.branchLine = sourceLineOf(program, b.end - 1);
        const std::string what = "the loop at lines " + std::to_string(r.firstLine) + "-" + std::to_string(r.branchLine);
        if (r.firstLine == 0 || r.branchLine <= r.firstLine || r.branchLine > lines.size())
        {
            return fail("the source lines of instructions " + std::to_string(b.begin) + "-" +
                        std::to_string(b.end - 1) + " are not known");
        }
        for (size_t i = 0; i < words.size(); ++i)
        {
            const unsigned line = sourceLineOf(program, i);
            const bool onLoopLines = line >= r.firstLine && line <= r.branchLine;
            const bool inLoop = i >= b.begin && i < b.end;
            if (onLoopLines != inLoop)
            {
                return fail(what + " share their lines with other instructions");
            }
        }
        r.branch = unroll_detail::parseLine(lines[r.branchLine - 1]);
        if (!r.branch.labels.empty() ||
            static_cast<int>(r.branch.operands.size()) != branchOperandCount(r.branch.mnemonic))
        {
            return fail(what + ": the back-branch is not written as a plain conditional branch");
        }
        bool targetsHead = false;
        for (const auto &[name, sym] : program.symbol_table)
        {
            targetsHead = targetsHead || (!sym.isData && sym.address == 4ull * b.begin && name == r.branch.operands.back());
        }
        if (!targetsHead)
        {
            return fail(what + ": the back-branch does not name the loop's label");
        }
        for (unsigned line = r.firstLine; line < r.branchLine; ++line)
        {
            const SourceLine p = unroll_detail::parseLine(lines[line - 1]);
            if (!isCode[line - 1] || (line != r.firstLine && !p.labels.empty()) || (!p.code.empty() && p.code[0] == '.'))
            {
                return fail(what + ": line " + std::to_string(line) + " has a label or a directive inside the loop");
            }
        }
        for (const Region &other : regions)
        {
            if (r.firstLine <= other.branchLine && other.firstLine <= r.branchLine)
            {
                return fail(what + " is listed twice");
            }
        }
        r.exitLabel = exitLabelName(r.branchLine, names);
        regions.push_back(r);
    }

    // Bottom-up, so the line numbers of the loops above stay valid.
    std::vector<size_t> order(regions.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&regions](size_t a, size_t b) { return regions[a].branchLine > regions[b].branchLine; });
    for (size_t idx : order)
    {
        const Region &r = regions[idx];
        const auto bodyBegin = lines.begin() + static_cast<std::ptrdiff_t>(r.firstLine - 1);
        const auto branchAt = lines.begin() + static_cast<std::ptrdiff_t>(r.branchLine - 1);
        const std::vector<std::string> body(bodyBegin, branchAt);
        const std::string branchLine = *branchAt;

        std::string keptOperands;
        for (size_t o = 0; o + 1 < r.branch.operands.size(); ++o)
        {
            keptOperands += r.branch.operands[o] + ", ";
        }
        std::vector<std::string> unrolled;
        for (int copy = 1; copy <= r.k; ++copy)
        {
            if (copy > 1)
            {
                unrolled.push_back("    # unrolled copy " + std::to_string(copy) + " of " + std::to_string(r.k) +
                                   " (lines " + std::to_string(r.firstLine) + "-" + std::to_string(r.branchLine) + ")");
            }
            for (size_t j = 0; j < body.size(); ++j)
            {
                const SourceLine p = unroll_detail::parseLine(body[j]);
                if (copy > 1 && j == 0 && !p.labels.empty())
                {
                    // Only the first copy keeps the loop's labels.
                    unrolled.push_back("    " + p.code + (p.comment.empty() ? "" : "  " + p.comment));
                }
                else
                {
                    unrolled.push_back(body[j]);
                }
            }
            if (copy < r.k)
            {
                unrolled.push_back("    " + invertedBranch(r.branch.mnemonic) + " " + keptOperands + r.exitLabel +
                                   "  # unrolled: leave after copy " + std::to_string(copy));
            }
        }
        unrolled.push_back(branchLine);
        unrolled.push_back(r.exitLabel + ":");
        lines.erase(bodyBegin, branchAt + 1);
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(r.firstLine - 1), unrolled.begin(), unrolled.end());
    }

    std::ostringstream text;
    for (const std::string &line : lines)
    {
        text << line << "\n";
    }
    out.ok = true;
    out.text = text.str();
    for (const Region &r : regions)
    {
        out.exitLabels.push_back(r.exitLabel);
    }
    return out;
}

// Does the assembled `unrolled` program hold the copies unrollSource() was
// asked for? Every copy's body word for word as in `original`; after copies
// 1 .. k-1 a branch with the same registers and the opposite condition to the
// instruction after the loop; after copy k the original branch back to the
// loop's first instruction; everything else shifted by the inserted words.
inline bool unrolledShapeMatches(const AssembledProgram &original, const AssembledProgram &unrolled,
                                 const std::vector<LoopToUnroll> &loops)
{
    std::vector<LoopToUnroll> sorted = loops;
    std::sort(sorted.begin(), sorted.end(),
              [](const LoopToUnroll &a, const LoopToUnroll &b) { return a.block.begin < b.block.begin; });
    const std::vector<uint32_t> &before = original.text_buffer;
    const std::vector<uint32_t> &after = unrolled.text_buffer;
    size_t added = 0;
    for (const LoopToUnroll &l : sorted)
    {
        added += static_cast<size_t>(l.k - 1) * (l.block.end - l.block.begin);
    }
    if (after.size() != before.size() + added)
    {
        return false;
    }

    size_t shift = 0;
    for (const LoopToUnroll &l : sorted)
    {
        const size_t length = l.block.end - l.block.begin;   // body + back-branch
        const size_t head = l.block.begin + shift;
        const uint32_t branch = before[l.block.end - 1];
        for (int copy = 0; copy < l.k; ++copy)
        {
            const size_t at = head + static_cast<size_t>(copy) * length;
            for (size_t j = 0; j + 1 < length; ++j)
            {
                if (after[at + j] != before[l.block.begin + j])
                {
                    return false;
                }
            }
            const size_t branchAt = at + length - 1;
            const uint32_t w = after[branchAt];
            const bool sameRegisters = ((w >> 15) & 0x1F) == ((branch >> 15) & 0x1F) &&
                                       ((w >> 20) & 0x1F) == ((branch >> 20) & 0x1F);
            if ((w & 0x7F) != 0x63 || !sameRegisters)
            {
                return false;
            }
            const uint32_t funct3 = (w >> 12) & 0x7;
            const uint32_t originalFunct3 = (branch >> 12) & 0x7;
            const int64_t target = static_cast<int64_t>(branchAt) + branchImm(w) / 4;
            const bool last = copy + 1 == l.k;
            const int64_t expected = last ? static_cast<int64_t>(head)
                                          : static_cast<int64_t>(head + static_cast<size_t>(l.k) * length);
            // beq/bne, blt/bge and bltu/bgeu differ only in the low funct3 bit.
            if (funct3 != (last ? originalFunct3 : (originalFunct3 ^ 1u)) || target != expected)
            {
                return false;
            }
        }
        shift += static_cast<size_t>(l.k - 1) * length;
    }
    return true;
}

// `report` with the safety net's words: a block the net rejected keeps its
// original order, so its moves are dropped too (writeAssembly() reads both).
inline ScheduleReport reportWithWords(const ScheduleReport &report, const std::vector<uint32_t> &words)
{
    ScheduleReport r = report;
    r.words = words;
    r.moves.clear();
    for (const Move &m : report.moves)
    {
        for (const BlockStalls &b : report.blocks)
        {
            if (m.newIndex < b.block.begin || m.newIndex >= b.block.end)
            {
                continue;
            }
            bool accepted = true;
            for (size_t i = b.block.begin; i < b.block.end; ++i)
            {
                accepted = accepted && words[i] == report.words[i];
            }
            if (accepted)
            {
                r.moves.push_back(m);
            }
            break;
        }
    }
    return r;
}

// A program after the existing reorder pipeline (Optimal, then the VM safety
// net), and its run.
struct ReorderedProgram
{
    ScheduleReport report;   // with the safety net's words
    Kites::RunResult run;
};

template <class VM>
ReorderedProgram reorderWithSafetyNet(const AssembledProgram &program, PipelineKind kind, const Seed &seed,
                                      unsigned cycleCap)
{
    const ScheduleReport report =
        scheduleProgram(program.text_buffer, kind, labelLeaders(program), true, ScheduleMode::Optimal);
    const SafetyNetResult net = applyWithVmCheck<VM>(program, report, seed, cycleCap);
    ReorderedProgram r;
    r.report = reportWithWords(report, net.words);
    r.run = runProgram<VM>(withText(program, net.words), seed, cycleCap, reorder_report::kDataWindowWords);
    return r;
}

// One unrolled program, built and checked against the original.
struct UnrollTrial
{
    bool ran = false;            // assembled, reordered and run
    bool passed = false;         // same final state and retired count, and faster than reorder-only
    std::string reason;          // why it did not pass
    std::string source;          // the unrolled source
    AssembledProgram program;    // assembled from it
    ReorderedProgram reordered;  // after Optimal + safety net
};

template <class VM>
UnrollTrial tryUnrolling(const std::string &source, const AssembledProgram &program,
                         const std::vector<LoopToUnroll> &loops, const Seed &seed, PipelineKind kind,
                         unsigned cycleCap, const Kites::RunResult &original, const Kites::RunResult &reorderOnly)
{
    UnrollTrial t;
    const UnrolledSource text = unrollSource(source, program, loops);
    if (!text.ok)
    {
        t.reason = text.reason;
        return t;
    }
    t.source = text.text;
    if (!bench_eval::assemblerHangLabel(t.source).empty())
    {
        t.reason = "the unrolled source has a label the assembler hangs on";
        return t;
    }
    try
    {
        std::istringstream in(t.source);
        t.program = assemble(in);
    }
    catch (const std::exception &e)
    {
        t.reason = std::string("the assembler rejected the unrolled source (") + e.what() + ")";
        return t;
    }
    if (!unrolledShapeMatches(program, t.program, loops))
    {
        t.reason = "the assembled copies do not match the loop";
        return t;
    }
    t.reordered = reorderWithSafetyNet<VM>(t.program, kind, seed, cycleCap);
    t.ran = true;
    const Kites::RunResult &run = t.reordered.run;
    if (run.hitCap)
    {
        t.reason = "it hit the cycle cap";
    }
    else if (!run.textIntact)
    {
        t.reason = "it overwrote its own code";
    }
    else if (!sameState(original, run))
    {
        t.reason = "its final state differs from the original's";
    }
    else if (run.retired != original.retired)
    {
        t.reason = "its retired count differs from the original's";
    }
    else if (run.cycles >= reorderOnly.cycles)
    {
        t.reason = "it is not faster than reorder-only";
    }
    else
    {
        t.passed = true;
    }
    return t;
}

// What happened to one eligible loop.
struct LoopUnrollRecord
{
    size_t block = 0;            // index into the original program's blocks
    unsigned firstLine = 0;      // source line of the loop's first instruction
    unsigned lastLine = 0;       // source line of its back-branch
    int k = 1;                   // copies chosen; 1 = no predicted gain
    int64_t predicted = 0;       // predicted cycles saved at k (reorder-only order in every copy)
    int64_t predicted2 = 0;      // ... at k = 2
    int64_t predicted4 = 0;      // ... at k = 4
    bool ran = false;            // unrolled on its own, reordered and run
    int64_t measured = 0;        // reorder-only cycles minus this loop alone unrolled + reordered (when ran)
    bool passed = false;         // passed on its own
    bool kept = false;           // part of the final program
    std::string reason;          // why it is not kept
};

// A loop (a backward branch or jump) that is not eligible, and why.
struct IneligibleLoop
{
    unsigned firstLine = 0;
    unsigned lastLine = 0;
    std::string reason;
};

struct UnrollOutcome
{
    PipelineKind kind = PipelineKind::HF;
    Kites::RunResult original;               // the program as given
    Kites::RunResult reorderOnly;            // Optimal + safety net
    Kites::RunResult final_;                 // what is kept
    bool unrolled = false;                   // final_ is an unrolled program; else it is reorder-only
    std::string note;                        // how the final program was chosen
    std::vector<LoopUnrollRecord> loops;     // every eligible loop, in program order
    std::vector<IneligibleLoop> ineligible;  // every other loop
    std::string finalSource;                 // the kept program's source (the original for reorder-only)
    AssembledProgram finalProgram;           // assembled from finalSource
    ScheduleReport finalReport;              // its reordering, with the safety net's words
};

// Every backward branch or j (findLoops() in block_entries.h, without calls:
// a jal that links a register) that does not close an eligible loop, with the
// reason.
inline std::vector<IneligibleLoop> ineligibleLoops(const AssembledProgram &program, const std::vector<Block> &blocks)
{
    const std::vector<uint32_t> &words = program.text_buffer;
    std::vector<size_t> blockOf(words.size(), 0);
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        for (size_t i = blocks[b].begin; i < blocks[b].end; ++i)
        {
            blockOf[i] = b;
        }
    }
    std::vector<IneligibleLoop> out;
    std::set<std::pair<size_t, size_t>> seen;
    for (const LoopRange &l : findLoops(words))
    {
        const DecodedInst back = decode(words[l.last]);
        if (back.kind == Kind::Jal && back.rd != 0)
        {
            continue;   // a call to code above it, not a loop
        }
        const Block &b = blocks[blockOf[l.last]];
        if ((b.begin == l.first && b.end == l.last + 1 && isEligibleLoop(words, b)) || !seen.insert({l.first, l.last}).second)
        {
            continue;
        }
        bool controlInside = false;
        for (size_t i = l.first; i < l.last; ++i)
        {
            const Kind kind = decode(words[i]).kind;
            controlInside = controlInside || kind == Kind::Jal || kind == Kind::Jalr || kind == Kind::Ecall;
        }
        IneligibleLoop r;
        r.firstLine = sourceLineOf(program, l.first);
        r.lastLine = sourceLineOf(program, l.last);
        if (back.kind == Kind::Jal)
        {
            r.reason = "it jumps back with j";
        }
        else if (l.first == l.last)
        {
            r.reason = "it has no body";
        }
        else if (controlInside)
        {
            r.reason = "it contains a call, j, ret or ecall";
        }
        else if (blockOf[l.first] != blockOf[l.last])
        {
            r.reason = "it has more than one basic block";
        }
        else
        {
            r.reason = "it contains an auipc (la)";
        }
        out.push_back(r);
    }
    return out;
}

// Picks the final program from the loops that passed on their own (`passed`,
// indices into the records) and the combined program's trial (if any): the
// combined program when it passed, otherwise the single loop with the largest
// measured saving (the earlier loop on a tie). Returns the records to keep;
// empty means reorder-only.
inline std::vector<size_t> pickUnrolledLoops(const std::vector<LoopUnrollRecord> &records,
                                             const std::vector<size_t> &passed, bool combinedPassed)
{
    if (passed.size() <= 1 || combinedPassed)
    {
        return passed;
    }
    size_t best = passed.front();
    for (size_t i : passed)
    {
        if (records[i].measured > records[best].measured)
        {
            best = i;
        }
    }
    return {best};
}

// Predict -> transform -> verify for one program on one pipeline (see the top
// of this file). `source` is the text `program` was assembled from, and `seed`
// its starting state (the same one the safety net and the checks use).
template <class VM>
UnrollOutcome unrollAndReorder(const std::string &source, const AssembledProgram &program, const Seed &seed,
                               PipelineKind kind, unsigned cycleCap = bench_eval::kBenchCycleCap)
{
    UnrollOutcome o;
    o.kind = kind;
    const std::vector<Block> blocks = findBlocks(program.text_buffer, labelLeaders(program));
    o.original = runProgram<VM>(program, seed, cycleCap, reorder_report::kDataWindowWords);
    const ReorderedProgram base = reorderWithSafetyNet<VM>(program, kind, seed, cycleCap);
    o.reorderOnly = base.run;
    o.final_ = base.run;
    o.finalSource = source;
    o.finalProgram = program;
    o.finalReport = base.report;
    o.ineligible = ineligibleLoops(program, blocks);
    for (size_t b : eligibleLoops(program.text_buffer, blocks))
    {
        LoopUnrollRecord r;
        r.block = b;
        r.firstLine = sourceLineOf(program, blocks[b].begin);
        r.lastLine = sourceLineOf(program, blocks[b].end - 1);
        o.loops.push_back(r);
    }
    o.note = "reorder-only";
    if (o.loops.empty())
    {
        return o;
    }
    if (o.original.hitCap || !o.original.textIntact)
    {
        for (LoopUnrollRecord &r : o.loops)
        {
            r.reason = "the original program does not finish cleanly";
        }
        return o;
    }
    const ExecutedPath path = tracePath(program, seed, 10ull * cycleCap);
    if (!path.finished)
    {
        for (LoopUnrollRecord &r : o.loops)
        {
            r.reason = "the program could not be traced to its end";
        }
        return o;
    }

    // Predict, then try every loop with a predicted gain on its own.
    const std::vector<UnrollChoice> choices = chooseUnrolling(base.report.words, blocks, path, kind);
    std::vector<size_t> passed;
    std::vector<UnrollTrial> trials(o.loops.size());
    for (size_t i = 0; i < o.loops.size(); ++i)
    {
        LoopUnrollRecord &r = o.loops[i];
        for (const UnrollChoice &c : choices)
        {
            if (c.block == r.block)
            {
                r.k = c.k;
                r.predicted = c.saving();
                r.predicted2 = c.saving2;
                r.predicted4 = c.saving4;
            }
        }
        if (r.k == 1)
        {
            r.reason = "no predicted gain";
            continue;
        }
        trials[i] = tryUnrolling<VM>(source, program, {{blocks[r.block], r.k}}, seed, kind, cycleCap, o.original,
                                     o.reorderOnly);
        r.ran = trials[i].ran;
        r.measured = r.ran ? static_cast<int64_t>(o.reorderOnly.cycles) - trials[i].reordered.run.cycles : 0;
        r.passed = trials[i].passed;
        r.reason = trials[i].reason;
        if (r.passed)
        {
            passed.push_back(i);
        }
    }

    // Combine the loops that passed; check the combination like a single loop.
    UnrollTrial combined;
    if (passed.size() > 1)
    {
        std::vector<LoopToUnroll> all;
        for (size_t i : passed)
        {
            all.push_back({blocks[o.loops[i].block], o.loops[i].k});
        }
        combined = tryUnrolling<VM>(source, program, all, seed, kind, cycleCap, o.original, o.reorderOnly);
    }
    const std::vector<size_t> keep = pickUnrolledLoops(o.loops, passed, combined.passed);
    if (keep.empty())
    {
        return o;
    }
    const UnrollTrial &chosen = keep.size() > 1 ? combined : trials[keep.front()];
    o.unrolled = true;
    o.final_ = chosen.reordered.run;
    o.finalSource = chosen.source;
    o.finalProgram = chosen.program;
    o.finalReport = chosen.reordered.report;
    for (size_t i : keep)
    {
        o.loops[i].kept = true;
    }
    if (keep.size() > 1)
    {
        o.note = "unrolled " + std::to_string(keep.size()) + " loops together";
    }
    else if (passed.size() > 1)
    {
        o.note = "the combined program failed (" + combined.reason + "); kept the best single loop";
        for (size_t i : passed)
        {
            if (!o.loops[i].kept)
            {
                o.loops[i].reason = "passed on its own, but the combined program failed (" + combined.reason +
                                    ") and another loop saved more";
            }
        }
    }
    else
    {
        o.note = "unrolled 1 loop";
    }
    return o;
}

// One line per loop: "loop at lines X–Y unrolled ×k: predicted N cycles saved,
// measured M" for the loops kept, and why not for every other loop.
inline std::string unrollSuggestions(const UnrollOutcome &o)
{
    std::ostringstream out;
    auto lines = [](unsigned first, unsigned last)
    { return "loop at lines " + std::to_string(first) + "–" + std::to_string(last); };
    for (const LoopUnrollRecord &r : o.loops)
    {
        out << lines(r.firstLine, r.lastLine);
        if (r.kept)
        {
            out << " unrolled ×" << r.k << ": predicted " << r.predicted << " cycles saved, measured " << r.measured;
        }
        else if (r.k == 1)
        {
            out << " not unrolled: no predicted gain (×2 " << r.predicted2 << ", ×4 " << r.predicted4 << " cycles)";
        }
        else
        {
            out << " not unrolled (×" << r.k << " predicted " << r.predicted << " cycles saved";
            if (r.ran)
            {
                out << ", measured " << r.measured;
            }
            out << "): " << r.reason;
        }
        out << "\n";
    }
    for (const IneligibleLoop &l : o.ineligible)
    {
        out << lines(l.firstLine, l.lastLine) << " not unrolled: " << l.reason << "\n";
    }
    return out.str();
}

// The kept program as Kites assembly (writeAssembly() of the unrolled and
// reordered source, or of the reorder-only program when nothing was kept).
inline std::string writeUnrolledAssembly(const UnrollOutcome &o)
{
    return writeAssembly(o.finalSource, o.finalProgram, o.finalReport);
}

} // namespace Kites::reorder
