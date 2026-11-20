// Ablation ladder: what each optimisation adds, measured on the 54 valid
// benchmarks (the reorder-bench programs, validity filter and inputs) on
// rv5-h-f and rv5-h-nf. Measurement only; nothing here changes the library.
//
//   C0  original program
//   C1  Greedy, HazardModel::Textbook, ContextMode::ArrayOrder, auipc as a
//       barrier (every auipc index and the index after it as extra leaders)
//   C2  C1 with HazardModel::Exact
//   C3  C2 with ScheduleMode::Optimal
//   C4  C3 without the extra auipc leaders (auipc pinning)
//   C5  C4 with ContextMode::BlockEntries (default loop weight)
//   C6  C5 plus the VM safety net (applyWithVmCheck)
//   C7  unrollAndReorder
//
// Writes bench/results/ablation/ladder.csv and summary.md. DISABLED_ so the
// default test run does not get slower; run it with
//   ./kites_test --gtest_also_run_disabled_tests --gtest_filter='*Ladder*'

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/bench_eval.h"
#include "reorder/loop_unroll.h"
#include "reorder/path_replay.h"
#include "reorder/program_runner.h"
#include "reorder/scheduler.h"
#include "reorder/vm_safety_net.h"
#include "utils/utils.h"

using namespace Kites::reorder;
namespace be = Kites::bench_eval;

namespace
{

constexpr int kSteps = 8;
const char *kStepName[kSteps] = {"original",      "greedy+textbook", "+exact",       "+optimal",
                                 "+auipc_pinning", "+block_entries", "+safety_net", "+unrolling"};

struct Row
{
    std::string program, category;
    int pipe = 0;   // 0 rv5-h-f, 1 rv5-h-nf
    int step = 0;
    unsigned cycles = 0, retired = 0, stalls = 0;
    bool sameState = false;
    uint64_t instructions = 0, dataStallCycles = 0, flushCycles = 0, predictedExact = 0;
    int64_t predictedTextbook = -1;   // C0 and C6 only
};

// The repository root: the first directory at or above the working directory
// that has bench/programs.
std::filesystem::path repoRoot()
{
    std::filesystem::path dir = std::filesystem::current_path();
    for (int up = 0; up < 6; ++up)
    {
        if (std::filesystem::is_directory(dir / "bench" / "programs"))
        {
            return dir;
        }
        dir = dir.parent_path();
    }
    return {};
}

// Every auipc index and the index after it: as extra block leaders they leave
// auipc alone in its own block, so nothing moves across it (a barrier).
std::vector<size_t> auipcBarrierLeaders(const std::vector<uint32_t> &words)
{
    std::vector<size_t> leaders;
    for (size_t i = 0; i < words.size(); ++i)
    {
        if ((words[i] & 0x7F) == 0x17)
        {
            leaders.push_back(i);
            leaders.push_back(i + 1);
        }
    }
    return leaders;
}

template <class VM>
void ladder(const std::string &name, const std::string &category, const std::string &source,
            const Kites::AssembledProgram &program, const Kites::Seed &seed, int pipe, std::vector<Row> &rows)
{
    const PipelineKind kind = pipe == 0 ? PipelineKind::HF : PipelineKind::HNF;
    const unsigned cap = be::kBenchCycleCap;
    const unsigned window = Kites::reorder_report::kDataWindowWords;
    const std::vector<uint32_t> &words = program.text_buffer;
    const std::vector<size_t> labels = labelLeaders(program);
    std::vector<size_t> withBarriers = labels;
    for (size_t i : auipcBarrierLeaders(words))
    {
        withBarriers.push_back(i);
    }
    const int weight = kDefaultLoopWeight;

    std::array<std::vector<uint32_t>, 7> stepWords;
    stepWords[0] = words;
    stepWords[1] = scheduleProgram(words, kind, withBarriers, true, ScheduleMode::Greedy, ContextMode::ArrayOrder,
                                   weight, HazardModel::Textbook)
                       .words;
    stepWords[2] = scheduleProgram(words, kind, withBarriers, true, ScheduleMode::Greedy, ContextMode::ArrayOrder,
                                   weight, HazardModel::Exact)
                       .words;
    stepWords[3] = scheduleProgram(words, kind, withBarriers, true, ScheduleMode::Optimal, ContextMode::ArrayOrder,
                                   weight, HazardModel::Exact)
                       .words;
    stepWords[4] = scheduleProgram(words, kind, labels, true, ScheduleMode::Optimal, ContextMode::ArrayOrder, weight,
                                   HazardModel::Exact)
                       .words;
    const ScheduleReport c5 = scheduleProgram(words, kind, labels, true, ScheduleMode::Optimal,
                                              ContextMode::BlockEntries, weight, HazardModel::Exact);
    stepWords[5] = c5.words;
    stepWords[6] = applyWithVmCheck<VM>(program, c5, seed, cap).words;

    const Kites::RunResult original = Kites::runProgram<VM>(program, seed, cap, window);
    const ExecutedPath path = tracePath(program, seed, 10ull * cap);
    EXPECT_TRUE(path.finished) << name;
    const std::vector<Block> blocks = findBlocks(words, labels);

    auto record = [&](int step, const Kites::AssembledProgram &run, const ExecutedPath &trace,
                      const std::vector<Block> &runBlocks)
    {
        Row r;
        r.program = name;
        r.category = category;
        r.pipe = pipe;
        r.step = step;
        const Kites::RunResult m = Kites::runProgram<VM>(run, seed, cap, window);
        r.cycles = m.cycles;
        r.retired = m.retired;
        r.stalls = m.stalls;
        r.sameState = Kites::sameState(original, m) && m.textIntact;
        const ReplayResult exact = replayPath(run.text_buffer, runBlocks, trace, kind, HazardModel::Exact);
        r.instructions = exact.retired;
        r.dataStallCycles = exact.stallCycles;
        r.flushCycles = exact.flushCycles;
        r.predictedExact = exact.predictedCycles;
        if (step == 0 || step == 6)
        {
            r.predictedTextbook = static_cast<int64_t>(
                replayPath(run.text_buffer, runBlocks, trace, kind, HazardModel::Textbook).predictedCycles);
        }
        rows.push_back(r);
    };
    for (int step = 0; step < 7; ++step)
    {
        record(step, Kites::withText(program, stepWords[static_cast<size_t>(step)]), path, blocks);
    }

    const UnrollOutcome o = unrollAndReorder<VM>(source, program, seed, kind);
    const Kites::AssembledProgram unrolled = Kites::withText(o.finalProgram, o.finalReport.words);
    record(7, unrolled, tracePath(unrolled, seed, 10ull * cap),
           findBlocks(unrolled.text_buffer, labelLeaders(o.finalProgram)));
    EXPECT_EQ(rows.back().cycles, o.final_.cycles) << name;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n == 0 ? 0.0 : (n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0);
}

std::string fixed(double value, int digits)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

} // namespace

TEST(ReorderAblationLadder, DISABLED_Ladder)
{
    Kites::setupVmStateDirectory();
    const std::filesystem::path root = repoRoot();
    ASSERT_FALSE(root.empty()) << "run from inside the repository (bench/programs not found above "
                               << std::filesystem::current_path() << ")";
    std::vector<be::FilterRow> filter;
    const std::vector<be::UsableProgram> usable = be::filterPrograms(be::findPrograms(root / "bench" / "programs"), filter);
    ASSERT_EQ(usable.size(), 54u);

    std::vector<Row> rows;
    for (const be::UsableProgram &u : usable)
    {
        const be::FilterRow &f = filter[u.filterIndex];
        const std::string source = be::readFile(f.path);
        ladder<Kites::RV5StageProcessorHF>(f.name, f.category, source, u.program, u.seed, 0, rows);
        ladder<Kites::RV5StageProcessorHNF>(f.name, f.category, source, u.program, u.seed, 1, rows);
    }
    const char *pipeName[2] = {"rv5-h-f", "rv5-h-nf"};

    // ladder.csv
    const std::filesystem::path out = root / "bench" / "results" / "ablation";
    std::filesystem::create_directories(out);
    {
        std::ofstream csv(out / "ladder.csv");
        csv << "program,category,pipeline,step,step_name,cycles,retired,total_stalls,cpi,same_state,"
               "replay_instructions,replay_fill,replay_data_stall_cycles,replay_flush_cycles,"
               "replay_predicted_exact,replay_predicted_textbook\n";
        std::vector<const Row *> sorted;
        for (const Row &r : rows)
        {
            sorted.push_back(&r);
        }
        std::sort(sorted.begin(), sorted.end(), [](const Row *a, const Row *b)
                  { return std::tie(a->program, a->pipe, a->step) < std::tie(b->program, b->pipe, b->step); });
        for (const Row *r : sorted)
        {
            csv << r->program << ',' << r->category << ',' << pipeName[r->pipe] << ",C" << r->step << ','
                << kStepName[r->step] << ',' << r->cycles << ',' << r->retired << ',' << r->stalls << ','
                << fixed(static_cast<double>(r->cycles) / r->retired, 3) << ',' << (r->sameState ? "yes" : "no")
                << ',' << r->instructions << ",4," << r->dataStallCycles << ',' << r->flushCycles << ','
                << r->predictedExact << ',';
            if (r->predictedTextbook >= 0)
            {
                csv << r->predictedTextbook;
            }
            csv << '\n';
        }
    }

    // summary.md
    std::ostringstream md;
    md << "# Ablation ladder\n\n"
       << "Each step adds one thing to the step before. " << usable.size()
       << " valid benchmarks (the reorder-bench programs, validity filter and inputs), cycle cap "
       << be::kBenchCycleCap << ". Measured cycles come from the VM; predictions from the path replay "
          "(src/reorder/path_replay.h) along each program's executed path.\n\n"
       << "| step | adds |\n|---|---|\n";
    const std::string stepWhat[kSteps] = {
        "the original program",
        "Greedy scheduler, Textbook hazard model, ArrayOrder contexts, auipc as a barrier",
        "Exact hazard model (the HDU quirk)",
        "Optimal scheduler",
        "auipc pinned instead of a barrier",
        "block-entry contexts (loop weight " + std::to_string(kDefaultLoopWeight) + ")",
        "VM safety net (applyWithVmCheck)",
        "loop unrolling (unrollAndReorder)"};
    for (int s = 0; s < kSteps; ++s)
    {
        md << "| C" << s << " | " << stepWhat[s] << " |\n";
    }
    md << "\nReductions are per program, against C0. Data-stall cycles are the replay's stall cycles that "
          "cost a cycle (the VM's total_stalls_ also counts stalls of flushed instructions; see "
          "ladder.csv). MAPE is the mean absolute % error of the replay's predicted cycles against the "
          "measured cycles; Textbook predictions are made for C0 and C6 only.\n";

    std::ostringstream console;
    for (int p = 0; p < 2; ++p)
    {
        std::map<std::string, unsigned> c0;
        for (const Row &r : rows)
        {
            if (r.pipe == p && r.step == 0)
            {
                c0[r.program] = r.cycles;
            }
        }
        md << "\n## " << pipeName[p] << "\n\n"
           << "| step | cycles | data-stall cycles | flush cycles | CPI | faster / same / slower than C0 | "
              "mean reduction | median reduction | best program (reduction) | state mismatches | MAPE Exact | "
              "MAPE Textbook |\n"
           << "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        double absErrExactAll = 0, absErrTextbookAll = 0;
        int nExactAll = 0, nTextbookAll = 0;
        int exactHits = 0, textbookHits = 0;
        std::vector<std::string> textbookMisses, slowerThanC0;
        for (int s = 0; s < kSteps; ++s)
        {
            long long cycles = 0, retired = 0, dataStalls = 0, flush = 0;
            int faster = 0, same = 0, slower = 0;
            std::vector<double> reductions;
            std::string best;
            double bestReduction = -1e9;
            unsigned bestFrom = 0, bestTo = 0;
            std::vector<std::string> mismatches;
            double errExact = 0, errTextbook = 0;
            int nTextbook = 0, n = 0;
            for (const Row &r : rows)
            {
                if (r.pipe != p || r.step != s)
                {
                    continue;
                }
                ++n;
                cycles += r.cycles;
                retired += r.retired;
                dataStalls += static_cast<long long>(r.dataStallCycles);
                flush += static_cast<long long>(r.flushCycles);
                const unsigned base = c0[r.program];
                faster += r.cycles < base ? 1 : 0;
                same += r.cycles == base ? 1 : 0;
                slower += r.cycles > base ? 1 : 0;
                if (r.cycles > base)
                {
                    slowerThanC0.push_back(r.program + " at C" + std::to_string(s) + " (" + std::to_string(base) +
                                           " -> " + std::to_string(r.cycles) + ")");
                }
                exactHits += r.predictedExact == r.cycles ? 1 : 0;
                if (r.predictedTextbook >= 0)
                {
                    if (r.predictedTextbook == static_cast<int64_t>(r.cycles))
                    {
                        ++textbookHits;
                    }
                    else
                    {
                        textbookMisses.push_back(r.program + " at C" + std::to_string(s) + " (predicted " +
                                                 std::to_string(r.predictedTextbook) + ", measured " +
                                                 std::to_string(r.cycles) + ")");
                    }
                }
                const double reduction = 100.0 * (static_cast<double>(base) - r.cycles) / base;
                reductions.push_back(reduction);
                if (reduction > bestReduction || (reduction == bestReduction && r.program < best))
                {
                    bestReduction = reduction;
                    best = r.program;
                    bestFrom = base;
                    bestTo = r.cycles;
                }
                if (!r.sameState)
                {
                    mismatches.push_back(r.program);
                }
                const double e = 100.0 * std::fabs(static_cast<double>(r.predictedExact) - r.cycles) / r.cycles;
                errExact += e;
                absErrExactAll += e;
                ++nExactAll;
                if (r.predictedTextbook >= 0)
                {
                    const double t =
                        100.0 * std::fabs(static_cast<double>(r.predictedTextbook) - r.cycles) / r.cycles;
                    errTextbook += t;
                    absErrTextbookAll += t;
                    ++nTextbook;
                    ++nTextbookAll;
                }
            }
            double mean = 0;
            for (double v : reductions)
            {
                mean += v;
            }
            mean /= reductions.empty() ? 1 : static_cast<double>(reductions.size());
            std::string mismatchText = "none";
            if (!mismatches.empty())
            {
                mismatchText.clear();
                for (const std::string &m : mismatches)
                {
                    mismatchText += (mismatchText.empty() ? "" : ", ") + m;
                }
            }
            md << "| C" << s << " " << kStepName[s] << " | " << cycles << " | " << dataStalls << " | " << flush << " | "
               << fixed(static_cast<double>(cycles) / retired, 3) << " | " << faster << " / " << same << " / "
               << slower << " | " << fixed(mean, 1) << "% | " << fixed(median(reductions), 1) << "% | "
               << (s == 0 ? "-" : best + " (" + fixed(bestReduction, 1) + "%, " + std::to_string(bestFrom) + " -> " +
                                      std::to_string(bestTo) + ")")
               << " | " << mismatchText << " | " << fixed(errExact / n, 2) << "% | "
               << (nTextbook > 0 ? fixed(errTextbook / nTextbook, 2) + "%" : "-") << " |\n";
            console << "[LADDER] " << pipeName[p] << " C" << s << " " << kStepName[s] << ": cycles " << cycles
                    << ", data stalls " << dataStalls << ", CPI " << fixed(static_cast<double>(cycles) / retired, 3)
                    << ", vs C0 " << faster << "/" << same << "/" << slower << ", mean " << fixed(mean, 1)
                    << "%, median " << fixed(median(reductions), 1) << "%, best " << best << " "
                    << fixed(bestReduction, 1) << "%, mismatches " << mismatchText << ", MAPE exact "
                    << fixed(errExact / n, 2) << "%"
                    << (nTextbook > 0 ? ", textbook " + fixed(errTextbook / nTextbook, 2) + "%" : "") << "\n";
            if (s == 0 || s == 6 || s == 7)
            {
                const long long anchor[2][3] = {{5465, 5313, 5064}, {8249, 7016, 6791}};
                EXPECT_EQ(cycles, anchor[p][s == 0 ? 0 : s == 6 ? 1 : 2]) << pipeName[p] << " C" << s;
            }
        }
        auto list = [](const std::vector<std::string> &items)
        {
            std::string text;
            for (const std::string &item : items)
            {
                text += (text.empty() ? "" : "; ") + item;
            }
            return text.empty() ? std::string("none") : text;
        };
        md << "\nOver all steps: MAPE Exact " << fixed(absErrExactAll / nExactAll, 2) << "% (" << nExactAll
           << " program runs); MAPE Textbook " << fixed(absErrTextbookAll / nTextbookAll, 2) << "% (" << nTextbookAll
           << " runs, C0 and C6).\n\n"
           << "Exact predictions equal the measured cycles in " << exactHits << " of " << nExactAll
           << " runs. Textbook predictions equal them in " << textbookHits << " of " << nTextbookAll
           << "; misses: " << list(textbookMisses) << ".\n\n"
           << "Slower than C0: " << list(slowerThanC0) << ".\n";
    }
    std::ofstream(out / "summary.md") << md.str();
    std::cout << console.str() << "[LADDER] wrote " << (out / "ladder.csv").string() << " and summary.md ("
              << rows.size() << " rows)" << std::endl;
}
