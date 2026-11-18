// Stage 2 end-to-end: every hazard program goes through the real scheduler (scheduleProgram), and
// the original and reordered programs both run in-process on rv5-h-f and rv5-h-nf. The VM run is
// the judge: same final state, never more cycles, and the scheduler's stall prediction must match
// what the VM measured, both before and after.
#include <gtest/gtest.h>

#include <iostream>
#include <utility>
#include <vector>

#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/dep_graph.h"
#include "reorder/scheduler.h"
#include "reorder_hazard_cases.h"

using namespace Kites;
using namespace Kites::hazard_bench;
using namespace Kites::reorder;

namespace
{
struct Stage2Tally
{
    unsigned improvedCount = 0;
    unsigned unchangedCount = 0;
    unsigned worseCount = 0;
    long long cyclesSaved = 0;
};

// The schedule is made for the pipeline it will run on: PipelineKind::HF for h_f, HNF for h_nf.
template <class VM>
void runStage2Case(const HazardCase &c, PipelineKind kind, const char *vmName, Stage2Tally &tally)
{
    SCOPED_TRACE(std::string(c.name) + " on " + vmName);
    AssembledProgram original = assembleHazardText(c.source);
    ScheduleReport report = scheduleProgram(original.text_buffer, kind, labelLeaders(original));
    ASSERT_EQ(report.words.size(), original.text_buffer.size());
    AssembledProgram reordered = withText(original, report.words);

    RunResult before = runProgram<VM>(original, hazardSeed(), kCycleCap);
    RunResult after = runProgram<VM>(reordered, hazardSeed(), kCycleCap);

    EXPECT_FALSE(before.hitCap);
    EXPECT_FALSE(after.hitCap);
    bool equal = sameState(before, after);
    EXPECT_TRUE(equal);
    EXPECT_EQ(before.retired, after.retired);
    EXPECT_LE(after.cycles, before.cycles);

    // The scheduler's own prediction must agree with the VM, for both versions.
    EXPECT_EQ(report.stallsBefore, static_cast<int>(before.stalls)) << "predicted vs measured, original";
    EXPECT_EQ(report.stallsAfter, static_cast<int>(after.stalls)) << "predicted vs measured, reordered";

    std::cerr << std::dec << "[STAGE2] " << c.name << " | " << vmName << " | stalls " << before.stalls << "->"
              << after.stalls << " | cycles " << before.cycles << "->" << after.cycles
              << " | equal=" << (equal ? "yes" : "no") << "\n";

    if (after.cycles < before.cycles)
    {
        ++tally.improvedCount;
    }
    else if (after.cycles == before.cycles)
    {
        ++tally.unchangedCount;
    }
    else
    {
        ++tally.worseCount;
    }
    tally.cyclesSaved += static_cast<long long>(before.cycles) - static_cast<long long>(after.cycles);
}

// First (producer, consumer) pair where the consumer really reads the producer's rd.
std::pair<size_t, size_t> findRawPair(const std::vector<uint32_t> &words)
{
    for (size_t i = 0; i < words.size(); ++i)
    {
        DecodedInst producer = decode(words[i]);
        if (!producer.writes_rd || producer.rd == 0)
        {
            continue;
        }
        for (size_t j = i + 1; j < words.size(); ++j)
        {
            DecodedInst consumer = decode(words[j]);
            if ((consumer.reads_rs1 && consumer.rs1 == producer.rd) ||
                (consumer.reads_rs2 && consumer.rs2 == producer.rd))
            {
                return {i, j};
            }
        }
    }
    return {words.size(), words.size()};
}
} // namespace

TEST(ReorderStage2Pipeline, ScheduledHazardProgramsKeepStateAndNeverSlowDown)
{
    Stage2Tally tally;
    unsigned caseCount = 0;
    for (const auto &c : kHazardCases)
    {
        runStage2Case<RV5StageProcessorHF>(c, PipelineKind::HF, "rv5-h-f", tally);
        runStage2Case<RV5StageProcessorHNF>(c, PipelineKind::HNF, "rv5-h-nf", tally);
        caseCount += 2;
    }
    std::cerr << "[STAGE2] summary | cases=" << caseCount << " (programs x VMs) | improved="
              << tally.improvedCount << " | unchanged=" << tally.unchangedCount
              << " | worse=" << tally.worseCount << " | total cycles saved=" << tally.cyclesSaved
              << "\n";
    EXPECT_EQ(tally.worseCount, 0u);
}

// Negative control: take the scheduler's real output, swap a producer with its consumer, and the
// equivalence check must notice.
TEST(ReorderStage2Pipeline, CorruptedScheduleIsDetected)
{
    AssembledProgram original = assembleHazardText("lw x5, 0(x10)\n"
                                                   "add x6, x5, x5\n"
                                                   "addi x7, x0, 32\n");
    ScheduleReport report =
        scheduleProgram(original.text_buffer, PipelineKind::HF, labelLeaders(original));

    std::vector<uint32_t> corrupted = report.words;
    auto [producerIdx, consumerIdx] = findRawPair(corrupted);
    ASSERT_LT(consumerIdx, corrupted.size()) << "no dependent pair found in the schedule";
    Block whole{0, corrupted.size(), false};
    ASSERT_TRUE(buildDepGraph(corrupted, whole).hasEdge(producerIdx, consumerIdx));
    std::swap(corrupted[producerIdx], corrupted[consumerIdx]);

    RunResult good = runProgram<RV5StageProcessorHF>(withText(original, report.words), hazardSeed(),
                                                     kCycleCap);
    RunResult bad =
        runProgram<RV5StageProcessorHF>(withText(original, corrupted), hazardSeed(), kCycleCap);
    EXPECT_FALSE(bad.hitCap);
    EXPECT_FALSE(sameState(good, bad));
}
