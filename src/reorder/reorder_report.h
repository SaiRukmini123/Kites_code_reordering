#pragma once
// Everything `kites-cli reorder` does, as testable functions: seed parsing, scheduling + running
// the original and reordered program, and the three outputs (text summary, self-contained HTML
// report, reordered assembly that keeps the source's labels, directives and comments).
//
// The scheduler itself is A's (reorder/scheduler.h, reorder/asm_writer.h); this file only calls it.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/asm_writer.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"

namespace Kites::reorder_report
{
using reorder::PipelineKind;
using reorder::ScheduleReport;

constexpr unsigned kDefaultCycleCap = 1000000;
constexpr unsigned kDataWindowWords = 64; // what sameState compares, from the data-section start

// ---------------------------------------------------------------------------------------------
// Seeds: "x10=0x10000000,mem:0x10000000=7"
// ---------------------------------------------------------------------------------------------

inline std::string trimSpaces(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return "";
    }
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// Decimal, 0x hex or 0 octal (std::stoull base 0); a leading '-' wraps to two's complement.
inline uint64_t parseSeedNumber(const std::string &text, const std::string &item)
{
    const std::string t = trimSpaces(text);
    size_t usedCount = 0;
    uint64_t value = 0;
    try
    {
        if (!t.empty() && t[0] == '-')
        {
            value = static_cast<uint64_t>(std::stoll(t, &usedCount, 0));
        }
        else
        {
            value = std::stoull(t, &usedCount, 0);
        }
    }
    catch (const std::exception &)
    {
        usedCount = 0;
    }
    if (t.empty() || usedCount != t.size())
    {
        throw std::invalid_argument("bad number '" + t + "' in seed item '" + item + "'");
    }
    return value;
}

inline Seed parseSeed(const std::string &spec)
{
    Seed seed;
    std::stringstream stream(spec);
    std::string rawItem;
    while (std::getline(stream, rawItem, ','))
    {
        const std::string item = trimSpaces(rawItem);
        if (item.empty())
        {
            continue;
        }
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
        {
            throw std::invalid_argument("seed item '" + item + "' has no '='");
        }
        const std::string key = trimSpaces(item.substr(0, eq));
        const uint64_t value = parseSeedNumber(item.substr(eq + 1), item);

        if (key.rfind("mem:", 0) == 0)
        {
            const uint64_t address = parseSeedNumber(key.substr(4), item);
            if (address % 4 != 0)
            {
                throw std::invalid_argument("seed address in '" + item + "' is not word aligned");
            }
            const bool fitsUnsigned = value <= 0xFFFFFFFFull;
            const bool fitsNegative = value >= 0xFFFFFFFF80000000ull;
            if (!fitsUnsigned && !fitsNegative)
            {
                throw std::invalid_argument("seed value in '" + item + "' does not fit in 32 bits");
            }
            seed.memWords.push_back({address, static_cast<uint32_t>(value)});
        }
        else if (key.size() >= 2 && key[0] == 'x' &&
                 key.find_first_not_of("0123456789", 1) == std::string::npos)
        {
            const int reg = std::stoi(key.substr(1));
            if (reg < 1 || reg > 31)
            {
                throw std::invalid_argument("seed register in '" + item + "' must be x1..x31");
            }
            seed.regs.push_back({reg, value});
        }
        else
        {
            throw std::invalid_argument("seed item '" + item +
                                        "' must be xN=<value> or mem:<addr>=<value>");
        }
    }
    return seed;
}

// ---------------------------------------------------------------------------------------------
// Schedule + run
// ---------------------------------------------------------------------------------------------

struct ReorderOutcome
{
    PipelineKind kind = PipelineKind::HF;
    unsigned cycleCap = kDefaultCycleCap;
    AssembledProgram original;
    AssembledProgram reordered;
    ScheduleReport report;
    RunResult before;
    RunResult after;
    bool equivalent = false; // sameState(before, after): neither hit the cap, same regs + data
};

inline const char *vmName(PipelineKind kind)
{
    return kind == PipelineKind::HF ? "rv5-h-f" : "rv5-h-nf";
}

template <class VM>
ReorderOutcome runReorderOn(const AssembledProgram &program, PipelineKind kind, const Seed &seed,
                            unsigned cycleCap)
{
    ReorderOutcome outcome;
    outcome.kind = kind;
    outcome.cycleCap = cycleCap;
    outcome.original = program;
    outcome.report =
        reorder::scheduleProgram(program.text_buffer, kind, reorder::labelLeaders(program));
    outcome.reordered = withText(program, outcome.report.words);
    outcome.before = runProgram<VM>(outcome.original, seed, cycleCap, kDataWindowWords);
    outcome.after = runProgram<VM>(outcome.reordered, seed, cycleCap, kDataWindowWords);
    outcome.equivalent = sameState(outcome.before, outcome.after);
    return outcome;
}

inline ReorderOutcome runReorder(const AssembledProgram &program, PipelineKind kind,
                                 const Seed &seed, unsigned cycleCap = kDefaultCycleCap)
{
    if (kind == PipelineKind::HF)
    {
        return runReorderOn<RV5StageProcessorHF>(program, kind, seed, cycleCap);
    }
    return runReorderOn<RV5StageProcessorHNF>(program, kind, seed, cycleCap);
}

// Predicted stall cycles charged to each instruction (the stalls spent before it can leave
// decode), from A's cost model: the growth of predictStalls over successive prefixes.
inline std::vector<int> predictedStallsPerInstruction(const std::vector<uint32_t> &words,
                                                      PipelineKind kind)
{
    std::vector<int> result(words.size(), 0);
    std::vector<uint32_t> prefix;
    prefix.reserve(words.size());
    int previousTotal = 0;
    for (size_t i = 0; i < words.size(); ++i)
    {
        prefix.push_back(words[i]);
        const int total = reorder::predictStalls(prefix, kind);
        result[i] = total - previousTotal;
        previousTotal = total;
    }
    return result;
}

inline std::string formatCpi(unsigned cycles, unsigned retired)
{
    if (retired == 0)
    {
        return "n/a";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << static_cast<double>(cycles) / retired;
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Text summary
// ---------------------------------------------------------------------------------------------

inline std::string writeTextSummary(const std::string &source, const ReorderOutcome &o)
{
    std::ostringstream out;
    out << std::dec;
    // A's report: predicted stalls per block, the minimal suggested moves (with source lines),
    // and every position change.
    out << reorder::writeSuggestions(source, o.original, o.report, o.kind);

    auto row = [&out](const std::string &name, const std::string &before, const std::string &after)
    {
        out << "  " << std::left << std::setw(22) << name << std::right << std::setw(10) << before
            << std::setw(12) << after << "\n";
    };
    out << "\nMeasured on " << vmName(o.kind) << " (cycle cap " << o.cycleCap << "):\n";
    row("", "original", "reordered");
    row("cycles", std::to_string(o.before.cycles), std::to_string(o.after.cycles));
    row("stall cycles", std::to_string(o.before.stalls), std::to_string(o.after.stalls));
    row("predicted stalls", std::to_string(o.report.stallsBefore),
        std::to_string(o.report.stallsAfter));
    row("instructions retired", std::to_string(o.before.retired), std::to_string(o.after.retired));
    row("CPI", formatCpi(o.before.cycles, o.before.retired),
        formatCpi(o.after.cycles, o.after.retired));
    out << "  cycles saved: "
        << static_cast<long long>(o.before.cycles) - static_cast<long long>(o.after.cycles) << "\n";

    if (o.before.hitCap || o.after.hitCap)
    {
        out << "\nWARNING: " << (o.before.hitCap ? "original" : "reordered")
            << " run hit the cycle cap; the comparison is not valid.\n";
    }
    out << "\nSame final state (x0-x31 and " << kDataWindowWords
        << " data words from the data-section start): " << (o.equivalent ? "yes" : "NO") << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Reordered assembly that keeps the source layout
// ---------------------------------------------------------------------------------------------

// A source line split into: leading whitespace + labels, the instruction, and everything after
// the instruction (spacing + comment).
struct SourceLineParts
{
    std::string prefix;
    std::string instruction;
    std::string suffix;
};

inline SourceLineParts splitSourceLine(const std::string &line)
{
    SourceLineParts parts;
    const size_t commentPos = std::min(line.find_first_of("#;"), line.size());
    size_t pos = line.find_first_not_of(" \t");
    pos = (pos == std::string::npos) ? line.size() : pos;

    // Labels: identifier followed by ':', possibly several.
    while (pos < commentPos)
    {
        size_t end = pos;
        while (end < commentPos &&
               (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_' ||
                line[end] == '.' || line[end] == '$'))
        {
            ++end;
        }
        if (end == pos || end >= commentPos || line[end] != ':')
        {
            break;
        }
        pos = line.find_first_not_of(" \t", end + 1);
        pos = (pos == std::string::npos) ? line.size() : std::min(pos, commentPos);
    }
    parts.prefix = line.substr(0, pos);

    size_t instEnd = commentPos;
    while (instEnd > pos && (line[instEnd - 1] == ' ' || line[instEnd - 1] == '\t'))
    {
        --instEnd;
    }
    parts.instruction = line.substr(pos, instEnd - pos);
    parts.suffix = line.substr(instEnd);
    return parts;
}

// The original source with only the instruction lines inside blocks rearranged. Each source line
// that produced instructions is a slot for the same instruction positions as before, and receives
// the instructions the scheduler put at those positions. An instruction keeps its own trailing
// comment (it describes the instruction, not the position); the slot keeps its leading
// indentation and labels. Full-line comments, labels, directives and blank lines are never
// touched, and branch targets keep their symbolic names, because the scheduler never moves an
// instruction across a block boundary.
//
// A pseudo-instruction that assembled to several words (la, li, ...) is written as its source
// text when its words are still together and in order. If the scheduler separated them (auipc is
// a block barrier, so the addi of an `la` can move), each word is written disassembled, with a
// "part k/n of line M" note.
inline std::string writeReorderedSource(const std::string &source, const AssembledProgram &program,
                                        const ScheduleReport &report)
{
    std::vector<std::string> lines = reorder::splitLines(source);
    const std::vector<size_t> original = reorder::originalIndexOf(report);

    std::map<unsigned, std::vector<size_t>> indicesOfLine;
    for (size_t i = 0; i < program.text_buffer.size(); ++i)
    {
        indicesOfLine[reorder::sourceLineOf(program, i)].push_back(i);
    }

    std::vector<std::string> output = lines;
    for (const auto &[lineNumber, positions] : indicesOfLine)
    {
        if (lineNumber == 0 || lineNumber > lines.size())
        {
            throw std::logic_error("instruction without a source line; cannot keep the layout");
        }
        std::vector<size_t> placed;
        for (size_t p : positions)
        {
            placed.push_back(original[p]);
        }
        if (placed == positions)
        {
            continue; // nothing moved into or out of this line
        }

        const SourceLineParts slot = splitSourceLine(lines[lineNumber - 1]);
        const std::string indent(slot.prefix.size(), ' ');
        std::ostringstream replaced;
        for (size_t k = 0; k < placed.size();)
        {
            replaced << (k == 0 ? slot.prefix : "\n" + indent);
            const unsigned fromLine = reorder::sourceLineOf(program, placed[k]);
            const std::vector<size_t> &whole = indicesOfLine[fromLine];
            const bool wholeLineHere =
                k + whole.size() <= placed.size() &&
                std::equal(whole.begin(), whole.end(), placed.begin() + static_cast<std::ptrdiff_t>(k));
            if (wholeLineHere)
            {
                const SourceLineParts from = splitSourceLine(lines[fromLine - 1]);
                replaced << from.instruction << from.suffix;
                k += whole.size();
                continue;
            }
            const size_t part = static_cast<size_t>(
                std::find(whole.begin(), whole.end(), placed[k]) - whole.begin()) + 1;
            replaced << reorder::disassembleForKites(report.words[positions[k]]) << "    # part "
                     << part << "/" << whole.size() << " of line " << fromLine << ": "
                     << reorder::stripComment(lines[fromLine - 1]);
            ++k;
        }
        output[lineNumber - 1] = replaced.str();
    }

    std::ostringstream out;
    for (size_t i = 0; i < output.size(); ++i)
    {
        out << output[i];
        if (i + 1 < output.size() || (!source.empty() && source.back() == '\n'))
        {
            out << "\n";
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Analysis for an interactive view (the GUI's "Suggest reordering" dialog): everything needed to
// draw both listings, computed without running a VM.
// ---------------------------------------------------------------------------------------------

struct BlockSummary
{
    unsigned firstLine = 0; // source lines of the block's first and last instruction
    unsigned lastLine = 0;
    int stallsBefore = 0; // predicted
    int stallsAfter = 0;
};

// What to mark in one listing: the source lines whose instruction slot changed, and the
// predicted stall cycles charged to each source line.
struct ListingMarks
{
    std::set<unsigned> changedLines;
    std::map<unsigned, int> stallsByLine;
};

struct ReorderAnalysis
{
    PipelineKind kind = PipelineKind::HF;
    ScheduleReport report;
    std::string reorderedSource;
    AssembledProgram reorderedProgram; // reorderedSource assembled (for its line mapping)
    bool roundTripOk = false;          // reorderedSource assembles to exactly report.words
    ListingMarks originalMarks;
    ListingMarks reorderedMarks;
    std::vector<BlockSummary> blocks;

    bool hasImprovement() const { return !report.moves.empty(); }
};

// `listing` is the program whose source is being shown (the original, or the reordered source
// assembled); `original[p]` is the original index of the instruction at position p.
inline ListingMarks listingMarks(const AssembledProgram &listing, const std::vector<size_t> &original,
                                 PipelineKind kind)
{
    ListingMarks marks;
    for (size_t p = 0; p < original.size(); ++p)
    {
        if (original[p] != p)
        {
            marks.changedLines.insert(reorder::sourceLineOf(listing, p));
        }
    }
    const std::vector<int> stalls = predictedStallsPerInstruction(listing.text_buffer, kind);
    for (size_t p = 0; p < stalls.size(); ++p)
    {
        if (stalls[p] > 0)
        {
            marks.stallsByLine[reorder::sourceLineOf(listing, p)] += stalls[p];
        }
    }
    return marks;
}

inline std::vector<BlockSummary> summarizeBlocks(const AssembledProgram &program,
                                                 const ScheduleReport &report)
{
    std::vector<BlockSummary> blocks;
    for (const reorder::BlockStalls &b : report.blocks)
    {
        if (b.block.end <= b.block.begin)
        {
            continue;
        }
        BlockSummary summary;
        summary.firstLine = reorder::sourceLineOf(program, b.block.begin);
        summary.lastLine = reorder::sourceLineOf(program, b.block.end - 1);
        summary.stallsBefore = b.stallsBefore;
        summary.stallsAfter = b.stallsAfter;
        blocks.push_back(summary);
    }
    return blocks;
}

// Schedules `program` (assembled from `source`) for `kind` and prepares both listings.
inline ReorderAnalysis analyzeReorder(const std::string &source, const AssembledProgram &program,
                                      PipelineKind kind)
{
    ReorderAnalysis analysis;
    analysis.kind = kind;
    analysis.report =
        reorder::scheduleProgram(program.text_buffer, kind, reorder::labelLeaders(program));
    analysis.reorderedSource = writeReorderedSource(source, program, analysis.report);
    analysis.blocks = summarizeBlocks(program, analysis.report);

    const std::vector<size_t> original = reorder::originalIndexOf(analysis.report);
    analysis.originalMarks = listingMarks(program, original, kind);
    try
    {
        std::istringstream stream(analysis.reorderedSource);
        analysis.reorderedProgram = assemble(stream);
        analysis.roundTripOk = analysis.reorderedProgram.text_buffer == analysis.report.words;
    }
    catch (const std::exception &)
    {
        analysis.roundTripOk = false;
    }
    if (analysis.roundTripOk)
    {
        analysis.reorderedMarks = listingMarks(analysis.reorderedProgram, original, kind);
    }
    return analysis;
}

// ---------------------------------------------------------------------------------------------
// HTML report (one self-contained file, inline CSS, light + dark)
// ---------------------------------------------------------------------------------------------

inline std::string htmlEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c; break;
        }
    }
    return out;
}

inline constexpr const char *kHtmlStyle = R"CSS(
:root {
  color-scheme: light dark;
  --bg: #f7f7f5; --panel: #ffffff; --text: #1d1d1b; --muted: #6b6b66; --border: #dedcd6;
  --moved-bg: #fff3c4; --moved-edge: #c99a06; --stall: #b4321f; --stall-bg: #fbe3de;
  --ok: #1e7a3c; --ok-bg: #dcf2e3; --bad: #b4321f; --bad-bg: #fbe3de; --warn: #8a5a00; --warn-bg: #fdf0d2;
  --label: #3b5bb5; --block: #c9c6be;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #161615; --panel: #1f1f1d; --text: #ecebe6; --muted: #a09f98; --border: #3a3935;
    --moved-bg: #4a3d0c; --moved-edge: #e2b42a; --stall: #ff8a75; --stall-bg: #4d1f17;
    --ok: #7ed79a; --ok-bg: #173822; --bad: #ff8a75; --bad-bg: #4d1f17; --warn: #f2c56b; --warn-bg: #42320f;
    --label: #93acf5; --block: #4a4944;
  }
}
* { box-sizing: border-box; }
body { margin: 0; padding: 24px 16px 48px; background: var(--bg); color: var(--text);
  font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
main { max-width: 1100px; margin: 0 auto; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 16px; margin: 28px 0 10px; }
.sub { color: var(--muted); margin: 0 0 16px; }
.badge { display: inline-block; padding: 6px 12px; border-radius: 6px; font-weight: 600; }
.badge.ok { color: var(--ok); background: var(--ok-bg); }
.badge.bad { color: var(--bad); background: var(--bad-bg); }
.badge.warn { color: var(--warn); background: var(--warn-bg); }
.panel { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; overflow-x: auto; }
table { border-collapse: collapse; width: 100%; }
th, td { padding: 4px 10px; text-align: left; white-space: nowrap; }
th { color: var(--muted); font-weight: 600; font-size: 13px; border-bottom: 1px solid var(--border); }
.metrics td.num, .metrics th.num { text-align: right; font-variant-numeric: tabular-nums; }
.metrics tr + tr td { border-top: 1px solid var(--border); }
.listing { font: 13px/1.6 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
.listing td.pos, .listing td.line { color: var(--muted); text-align: right; width: 1%; }
.listing td.gap { width: 24px; border-left: 1px solid var(--border); padding: 0; }
.listing tr.label td { color: var(--label); padding-top: 6px; }
.listing tr.block-start td { border-top: 1px dashed var(--block); }
.listing td.moved { background: var(--moved-bg); box-shadow: inset 3px 0 var(--moved-edge); }
.listing .from { color: var(--muted); font-size: 12px; margin-left: 8px; }
.stall { display: inline-block; min-width: 2.2em; padding: 0 6px; border-radius: 4px; text-align: center;
  color: var(--stall); background: var(--stall-bg); font-weight: 600; }
.legend { color: var(--muted); font-size: 13px; margin: 8px 0 0; }
.legend span { margin-right: 16px; }
.swatch { display: inline-block; width: 12px; height: 12px; vertical-align: -1px; margin-right: 4px;
  border-radius: 2px; background: var(--moved-bg); box-shadow: inset 3px 0 var(--moved-edge); }
)CSS";

inline std::string writeHtmlReport(const std::string &source, const ReorderOutcome &o,
                                   const std::string &title)
{
    const std::vector<std::string> lines = reorder::splitLines(source);
    const AssembledProgram &program = o.original;
    const ScheduleReport &report = o.report;
    const size_t n = report.words.size();
    const std::vector<size_t> original = reorder::originalIndexOf(report);
    const std::vector<int> stallsBefore = predictedStallsPerInstruction(program.text_buffer, o.kind);
    const std::vector<int> stallsAfter = predictedStallsPerInstruction(report.words, o.kind);
    const auto labels = reorder::textLabelsByIndex(program);

    std::set<size_t> movedOriginal;
    for (const reorder::Move &m : report.moves)
    {
        movedOriginal.insert(m.originalIndex);
    }
    std::set<size_t> blockStarts;
    for (const reorder::BlockStalls &b : report.blocks)
    {
        blockStarts.insert(b.block.begin);
    }

    std::map<unsigned, std::vector<size_t>> indicesOfLine;
    for (size_t i = 0; i < program.text_buffer.size(); ++i)
    {
        indicesOfLine[reorder::sourceLineOf(program, i)].push_back(i);
    }
    // Source text, plus "part k/n" for the words of a multi-word pseudo-instruction (la, li, ...).
    auto textOf = [&](size_t index)
    {
        const unsigned line = reorder::sourceLineOf(program, index);
        std::string text = htmlEscape(reorder::sourceTextOf(lines, line));
        const std::vector<size_t> &words = indicesOfLine[line];
        if (words.size() > 1)
        {
            const size_t part =
                static_cast<size_t>(std::find(words.begin(), words.end(), index) - words.begin()) + 1;
            text += "<span class=\"from\">part " + std::to_string(part) + "/" +
                    std::to_string(words.size()) + "</span>";
        }
        return text;
    };
    auto stallCell = [](int stalls)
    {
        if (stalls <= 0)
        {
            return std::string("<td></td>");
        }
        return "<td><span class=\"stall\" title=\"predicted stall cycles before this instruction\">+" +
               std::to_string(stalls) + "</span></td>";
    };

    std::ostringstream out;
    out << std::dec;
    out << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        << "<title>Reorder report</title>\n<style>" << kHtmlStyle << "</style>\n</head>\n<body>\n<main>\n";
    out << "<h1>Reorder report</h1>\n<p class=\"sub\">" << htmlEscape(title) << " on "
        << vmName(o.kind) << "</p>\n";

    // Verdict.
    if (o.before.hitCap || o.after.hitCap)
    {
        out << "<p><span class=\"badge warn\" id=\"verdict\">Inconclusive: a run hit the cycle cap ("
            << o.cycleCap << ")</span></p>\n";
    }
    else if (o.equivalent)
    {
        out << "<p><span class=\"badge ok\" id=\"verdict\">Equivalent: same final registers and "
               "data memory</span></p>\n";
    }
    else
    {
        out << "<p><span class=\"badge bad\" id=\"verdict\">NOT equivalent: final state differs"
               "</span></p>\n";
    }

    // Metrics.
    auto metric = [&out](const std::string &name, long long before, long long after)
    {
        out << "<tr><td>" << name << "</td><td class=\"num\">" << before << "</td><td class=\"num\">"
            << after << "</td><td class=\"num\">" << (after - before > 0 ? "+" : "")
            << (after - before) << "</td></tr>\n";
    };
    out << "<h2>" << vmName(o.kind) << "</h2>\n<div class=\"panel\"><table class=\"metrics\">\n"
        << "<tr><th></th><th class=\"num\">Original</th><th class=\"num\">Reordered</th>"
        << "<th class=\"num\">Change</th></tr>\n";
    metric("Cycles", o.before.cycles, o.after.cycles);
    metric("Stall cycles (measured)", o.before.stalls, o.after.stalls);
    metric("Stall cycles (predicted)", report.stallsBefore, report.stallsAfter);
    metric("Instructions retired", o.before.retired, o.after.retired);
    out << "<tr><td>CPI</td><td class=\"num\">" << formatCpi(o.before.cycles, o.before.retired)
        << "</td><td class=\"num\">" << formatCpi(o.after.cycles, o.after.retired)
        << "</td><td></td></tr>\n</table></div>\n";

    // Side-by-side listing: one row per instruction position, so both sides line up.
    out << "<h2>Listing</h2>\n<div class=\"panel\"><table class=\"listing\">\n"
        << "<tr><th colspan=\"4\" id=\"original-heading\">Original</th><th class=\"gap\"></th>"
        << "<th colspan=\"4\" id=\"reordered-heading\">Reordered</th></tr>\n"
        << "<tr><th>#</th><th>line</th><th>instruction</th><th>stall</th><th class=\"gap\"></th>"
        << "<th>#</th><th>line</th><th>instruction</th><th>stall</th></tr>\n";
    for (size_t pos = 0; pos <= n; ++pos)
    {
        auto labelIt = labels.find(pos);
        if (labelIt != labels.end())
        {
            std::string names;
            for (const std::string &name : labelIt->second)
            {
                names += htmlEscape(name) + ":  ";
            }
            out << "<tr class=\"label\"><td></td><td></td><td colspan=\"2\">" << names
                << "</td><td class=\"gap\"></td><td></td><td></td><td colspan=\"2\">" << names
                << "</td></tr>\n";
        }
        if (pos == n)
        {
            break;
        }
        const size_t from = original[pos];
        const bool leftMoved = movedOriginal.count(pos) != 0;
        const bool rightMoved = from != pos;
        out << "<tr" << (blockStarts.count(pos) && pos != 0 ? " class=\"block-start\"" : "") << ">"
            << "<td class=\"pos\">" << pos << "</td><td class=\"line\">"
            << reorder::sourceLineOf(program, pos) << "</td><td class=\"orig"
            << (leftMoved ? " moved" : "") << "\">" << textOf(pos) << "</td>" << stallCell(stallsBefore[pos])
            << "<td class=\"gap\"></td><td class=\"pos\">" << pos << "</td><td class=\"line\">"
            << reorder::sourceLineOf(program, from) << "</td><td class=\"reord"
            << (rightMoved ? " moved" : "") << "\">" << textOf(from);
        if (rightMoved)
        {
            out << "<span class=\"from\">from #" << from << "</span>";
        }
        out << "</td>" << stallCell(stallsAfter[pos]) << "</tr>\n";
    }
    out << "</table></div>\n"
        << "<p class=\"legend\"><span><i class=\"swatch\"></i>moved by the scheduler</span>"
        << "<span><b class=\"stall\">+N</b> predicted stall cycles before the instruction</span>"
        << "<span>dashed rule: basic-block boundary</span></p>\n"
        << "<p class=\"legend\">Equivalence compares x0-x31 and " << kDataWindowWords
        << " words from the data-section start after both runs finish.</p>\n"
        << "</main>\n</body>\n</html>\n";
    return out.str();
}

} // namespace Kites::reorder_report
