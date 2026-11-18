// Tests for reorder/reorder_report.h: the pieces behind `kites-cli reorder`.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "reorder/reorder_report.h"
#include "reorder_hazard_cases.h"

using namespace Kites;
using namespace Kites::hazard_bench;
using namespace Kites::reorder_report;
using Kites::reorder::PipelineKind;

namespace
{
size_t countOccurrences(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
    {
        ++count;
    }
    return count;
}

// load-use with an independent addi after it: the scheduler moves the addi into the gap.
const std::string kLoadUseSource = "# load-use demo\n"
                                   "start:\n"
                                   "    lw   x5, 0(x10)      # load\n"
                                   "    add  x6, x5, x5      # use\n"
                                   "    addi x7, x0, 32      # independent\n";
} // namespace

TEST(ReorderReport, HtmlContainsBothListingsAndVerdict)
{
    AssembledProgram program = assembleHazardText(kLoadUseSource);
    ReorderOutcome outcome = runReorder(program, PipelineKind::HF, hazardSeed(), kCycleCap);
    ASSERT_TRUE(outcome.equivalent);
    ASSERT_FALSE(outcome.report.moves.empty()) << "expected the scheduler to move the addi";

    const std::string html = writeHtmlReport(kLoadUseSource, outcome, "load_use.s");

    EXPECT_NE(html.find("id=\"original-heading\">Original<"), std::string::npos);
    EXPECT_NE(html.find("id=\"reordered-heading\">Reordered<"), std::string::npos);
    // Every instruction's source text is in both listings.
    for (const char *text : {"lw   x5, 0(x10)", "add  x6, x5, x5", "addi x7, x0, 32"})
    {
        EXPECT_EQ(countOccurrences(html, std::string(">") + text), 2u) << text;
    }
    EXPECT_NE(html.find("start:"), std::string::npos);
    EXPECT_NE(html.find("class=\"badge ok\" id=\"verdict\">Equivalent"), std::string::npos);
    EXPECT_NE(html.find(" moved\""), std::string::npos);
    EXPECT_NE(html.find("from #2"), std::string::npos);
    EXPECT_NE(html.find("class=\"stall\""), std::string::npos); // the original's load-use stall
    EXPECT_NE(html.find("rv5-h-f"), std::string::npos);
    EXPECT_NE(html.find("<td>CPI</td>"), std::string::npos);

    // Self-contained and themed.
    EXPECT_EQ(html.find("http://"), std::string::npos);
    EXPECT_EQ(html.find("https://"), std::string::npos);
    EXPECT_EQ(html.find("<script"), std::string::npos);
    EXPECT_EQ(html.find("<link"), std::string::npos);
    EXPECT_NE(html.find("prefers-color-scheme: dark"), std::string::npos);
}

TEST(ReorderReport, HtmlShowsNotEquivalentVerdict)
{
    AssembledProgram program = assembleHazardText(kLoadUseSource);
    ReorderOutcome outcome = runReorder(program, PipelineKind::HF, hazardSeed(), kCycleCap);

    // Replace the reordered run by a broken one (use before its load).
    std::vector<uint32_t> broken(program.text_buffer.begin(), program.text_buffer.end());
    std::swap(broken[0], broken[1]);
    outcome.after = runProgram<RV5StageProcessorHF>(withText(program, broken), hazardSeed(), kCycleCap);
    outcome.equivalent = sameState(outcome.before, outcome.after);
    ASSERT_FALSE(outcome.equivalent);

    const std::string html = writeHtmlReport(kLoadUseSource, outcome, "broken");
    EXPECT_NE(html.find("class=\"badge bad\" id=\"verdict\">NOT equivalent"), std::string::npos);
    EXPECT_EQ(html.find(">Equivalent"), std::string::npos);
}

TEST(ReorderReport, HtmlEscapesText)
{
    EXPECT_EQ(htmlEscape("a<b>&\"c'"), "a&lt;b&gt;&amp;&quot;c&#39;");
}

TEST(ReorderReport, ParseSeed)
{
    Seed seed = parseSeed("x10=0x10000000, mem:0x10000000=7 ,x11=-1");
    ASSERT_EQ(seed.regs.size(), 2u);
    EXPECT_EQ(seed.regs[0].first, 10);
    EXPECT_EQ(seed.regs[0].second, 0x10000000u);
    EXPECT_EQ(seed.regs[1].first, 11);
    EXPECT_EQ(seed.regs[1].second, ~0ull);
    ASSERT_EQ(seed.memWords.size(), 1u);
    EXPECT_EQ(seed.memWords[0].first, 0x10000000u);
    EXPECT_EQ(seed.memWords[0].second, 7u);

    EXPECT_TRUE(parseSeed("").regs.empty());
    EXPECT_THROW(parseSeed("x0=1"), std::invalid_argument);
    EXPECT_THROW(parseSeed("x32=1"), std::invalid_argument);
    EXPECT_THROW(parseSeed("a0=1"), std::invalid_argument);
    EXPECT_THROW(parseSeed("x10"), std::invalid_argument);
    EXPECT_THROW(parseSeed("x10=0x1g"), std::invalid_argument);
    EXPECT_THROW(parseSeed("mem:0x10000002=1"), std::invalid_argument);
    EXPECT_THROW(parseSeed("mem:0x10000000=0x100000000"), std::invalid_argument);
}

TEST(ReorderReport, ReorderedSourceKeepsLayoutAndReassembles)
{
    const std::string source = ".data\n"
                               "buf: .word 7, 0\n"
                               ".text\n"
                               "# two chains\n"
                               "main: addi x5, x0, 64   # chain A\n"
                               "    add  x6, x5, x5     # A uses A\n"
                               "\n"
                               "    addi x11, x0, 32    # chain B\n"
                               "    add  x12, x11, x11  # B uses B\n"
                               "    beq  x12, x0, done\n"
                               "    addi x13, x0, 32\n"
                               "done:\n"
                               "    addi x14, x0, 64\n";
    AssembledProgram program = assembleHazardText(source);
    ScheduleReport report =
        reorder::scheduleProgram(program.text_buffer, PipelineKind::HNF, reorder::labelLeaders(program));
    ASSERT_FALSE(report.moves.empty()) << "expected the chains to be interleaved";

    const std::string written = writeReorderedSource(source, program, report);
    const std::vector<std::string> before = reorder::splitLines(source);
    const std::vector<std::string> after = reorder::splitLines(written);
    ASSERT_EQ(before.size(), after.size());
    std::multiset<std::string> bodiesBefore, bodiesAfter;
    for (size_t i = 0; i < before.size(); ++i)
    {
        const reorder_report::SourceLineParts b = splitSourceLine(before[i]);
        const reorder_report::SourceLineParts a = splitSourceLine(after[i]);
        if (b.instruction.empty() || b.instruction[0] == '.')
        {
            EXPECT_EQ(after[i], before[i]) << "non-instruction line " << i + 1 << " changed";
            continue;
        }
        // The slot keeps its indentation and labels; instruction + trailing comment travel together.
        EXPECT_EQ(a.prefix, b.prefix) << "label/indent of line " << i + 1 << " changed";
        bodiesBefore.insert(b.instruction + b.suffix);
        bodiesAfter.insert(a.instruction + a.suffix);
    }
    EXPECT_EQ(bodiesAfter, bodiesBefore);
    EXPECT_NE(written.find("main: addi x5, x0, 64   # chain A"), std::string::npos) << written;
    EXPECT_NE(written.find("    beq  x12, x0, done"), std::string::npos); // symbolic target kept

    std::istringstream stream(written);
    EXPECT_EQ(assemble(stream).text_buffer, report.words);
}

// When the words of one pseudo-instruction are separated, the slot falls back to one
// disassembled instruction per word, and the file still assembles to the same words.
TEST(ReorderReport, ReorderedSourceSplitsSeparatedPseudoInstruction)
{
    const std::string source = "li x5, 0x12345678\n"
                               "addi x7, x0, 32\n";
    AssembledProgram program = assembleHazardText(source);
    ASSERT_EQ(program.text_buffer.size(), 3u) << "li should expand to lui + addi";

    ScheduleReport report;
    report.words = {program.text_buffer[0], program.text_buffer[2], program.text_buffer[1]};
    report.moves = {{2, 1}, {1, 2}};

    const std::string written = writeReorderedSource(source, program, report);
    EXPECT_NE(written.find("# part 2/2 of line 1: li x5, 0x12345678"), std::string::npos) << written;
    EXPECT_NE(written.find("addi x7, x0, 32"), std::string::npos) << written;
    std::istringstream stream(written);
    EXPECT_EQ(assemble(stream).text_buffer, report.words) << written;
}

TEST(ReorderReport, PredictedStallsPerInstruction)
{
    AssembledProgram program = assembleHazardText(kLoadUseSource);
    const std::vector<uint32_t> words(program.text_buffer.begin(), program.text_buffer.end());
    EXPECT_EQ(predictedStallsPerInstruction(words, PipelineKind::HF), (std::vector<int>{0, 1, 0}));
    EXPECT_EQ(predictedStallsPerInstruction(words, PipelineKind::HNF), (std::vector<int>{0, 2, 0}));
}

TEST(ReorderReport, TextSummaryHasCyclesCpiAndVerdict)
{
    AssembledProgram program = assembleHazardText(kLoadUseSource);
    ReorderOutcome outcome = runReorder(program, PipelineKind::HNF, hazardSeed(), kCycleCap);
    const std::string text = writeTextSummary(kLoadUseSource, outcome);
    EXPECT_NE(text.find("Measured on rv5-h-nf"), std::string::npos);
    EXPECT_NE(text.find("CPI"), std::string::npos);
    EXPECT_NE(text.find("Same final state"), std::string::npos);
    EXPECT_NE(text.find(": yes"), std::string::npos);
}

// analyzeReorder: what the GUI dialog draws.
TEST(ReorderReport, AnalysisMarksMovedLinesStallsAndBlocks)
{
    AssembledProgram program = assembleHazardText(kLoadUseSource);
    ReorderAnalysis analysis = analyzeReorder(kLoadUseSource, program, PipelineKind::HF);

    ASSERT_TRUE(analysis.hasImprovement());
    ASSERT_TRUE(analysis.roundTripOk);
    // kLoadUseSource: line 3 lw, line 4 add, line 5 addi. The addi moves into the load-use gap.
    EXPECT_EQ(analysis.originalMarks.changedLines, (std::set<unsigned>{4, 5}));
    EXPECT_EQ(analysis.reorderedMarks.changedLines, (std::set<unsigned>{4, 5}));
    EXPECT_EQ(analysis.originalMarks.stallsByLine, (std::map<unsigned, int>{{4, 1}}));
    EXPECT_TRUE(analysis.reorderedMarks.stallsByLine.empty());
    ASSERT_EQ(analysis.blocks.size(), 1u);
    EXPECT_EQ(analysis.blocks[0].firstLine, 3u);
    EXPECT_EQ(analysis.blocks[0].lastLine, 5u);
    EXPECT_EQ(analysis.blocks[0].stallsBefore, 1);
    EXPECT_EQ(analysis.blocks[0].stallsAfter, 0);
}

TEST(ReorderReport, AnalysisWithNothingToMove)
{
    const std::string source = "addi x5, x0, 64\nadd x6, x5, x5\nadd x7, x6, x6\n";
    AssembledProgram program = assembleHazardText(source);
    ReorderAnalysis analysis = analyzeReorder(source, program, PipelineKind::HNF);
    EXPECT_FALSE(analysis.hasImprovement());
    EXPECT_TRUE(analysis.roundTripOk);
    EXPECT_EQ(analysis.reorderedSource, source);
    EXPECT_TRUE(analysis.originalMarks.changedLines.empty());
    EXPECT_EQ(analysis.report.stallsBefore, analysis.report.stallsAfter);
}

// With a split `la`, the reordered listing has more lines than the original; its marks come from
// the reordered source's own line mapping, so per-line stalls still add up to the prediction.
TEST(ReorderReport, AnalysisStallMarksSumToPredictionWithSplitPseudo)
{
    const std::string source = ".data\n"
                               "vals: .word 7, 5\n"
                               ".text\n"
                               "    la   x10, vals\n"
                               "    lw   x5, 0(x10)\n"
                               "    add  x6, x5, x5\n"
                               "    lw   x7, 4(x10)\n"
                               "    add  x8, x7, x7\n"
                               "    addi x11, x0, 32\n"
                               "    addi x12, x0, 64\n";
    AssembledProgram program = assembleHazardText(source);
    for (PipelineKind kind : {PipelineKind::HF, PipelineKind::HNF})
    {
        ReorderAnalysis analysis = analyzeReorder(source, program, kind);
        ASSERT_TRUE(analysis.roundTripOk);
        int before = 0, after = 0;
        for (const auto &[line, stalls] : analysis.originalMarks.stallsByLine)
        {
            before += stalls;
        }
        for (const auto &[line, stalls] : analysis.reorderedMarks.stallsByLine)
        {
            after += stalls;
        }
        EXPECT_EQ(before, analysis.report.stallsBefore);
        EXPECT_EQ(after, analysis.report.stallsAfter);
        EXPECT_LT(analysis.report.stallsAfter, analysis.report.stallsBefore);
    }
}
