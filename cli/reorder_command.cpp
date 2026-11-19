/**
 * @file reorder_command.cpp
 * @brief `kites-cli reorder <file.s> --vm rv5-h-f|rv5-h-nf [--seed ...] [--html out.html] [--out reordered.s]`
 */
#include "reorder_command.h"

#include "assembler/assembler.h"
#include "reorder/bench_eval.h"
#include "reorder/fuzz.h"
#include "reorder/reorder_report.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace Kites
{
namespace
{
void printReorderUsage(const char *programName)
{
    std::cerr
        << "Usage: " << programName
        << " reorder <file.s> --vm rv5-h-f|rv5-h-nf [--seed <spec>] [--html <out.html>] "
           "[--out <reordered.s>]\n"
        << "  --vm      pipeline to schedule for and to run on (required)\n"
        << "  --seed    initial state, e.g. \"x10=0x10000000,mem:0x10000000=7\"\n"
        << "            (xN=<value> for x1..x31, mem:<addr>=<32-bit word>; dec or 0x hex)\n"
        << "  --html    write a self-contained HTML report\n"
        << "  --out     write the reordered assembly (labels, directives, comments kept)\n"
        << "Exit code: 0 equivalent, 1 usage/assembly/IO error, 4 not equivalent or cycle cap hit.\n";
}

bool readWholeFile(const std::string &path, std::string &contents)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    contents = buffer.str();
    return true;
}

bool writeWholeFile(const std::string &path, const std::string &contents)
{
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return static_cast<bool>(out);
}
} // namespace

int RunReorderCommand(int argc, char *argv[])
{
    std::string assemblyFile;
    std::string vmArg;
    std::string seedSpec;
    std::string htmlPath;
    std::string outPath;

    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto takeValue = [&](std::string &target)
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: " << arg << " requires a value.\n";
                return false;
            }
            target = argv[++i];
            return true;
        };

        if (arg == "--vm" || arg == "--seed" || arg == "--html" || arg == "--out")
        {
            std::string &target = arg == "--vm"     ? vmArg
                                  : arg == "--seed" ? seedSpec
                                  : arg == "--html" ? htmlPath
                                                    : outPath;
            if (!takeValue(target))
            {
                printReorderUsage(argv[0]);
                return 1;
            }
        }
        else if (arg == "--help" || arg == "-h")
        {
            printReorderUsage(argv[0]);
            return 0;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Error: unknown argument '" << arg << "'.\n";
            printReorderUsage(argv[0]);
            return 1;
        }
        else if (assemblyFile.empty())
        {
            assemblyFile = arg;
        }
        else
        {
            std::cerr << "Error: unexpected extra argument '" << arg << "'.\n";
            printReorderUsage(argv[0]);
            return 1;
        }
    }

    if (assemblyFile.empty())
    {
        std::cerr << "Error: reorder needs an assembly file.\n";
        printReorderUsage(argv[0]);
        return 1;
    }
    reorder::PipelineKind kind;
    if (vmArg == "rv5-h-f")
    {
        kind = reorder::PipelineKind::HF;
    }
    else if (vmArg == "rv5-h-nf")
    {
        kind = reorder::PipelineKind::HNF;
    }
    else
    {
        std::cerr << "Error: reorder needs --vm rv5-h-f or --vm rv5-h-nf"
                  << (vmArg.empty() ? "" : " (got '" + vmArg + "')") << ".\n";
        printReorderUsage(argv[0]);
        return 1;
    }

    Seed seed;
    try
    {
        seed = reorder_report::parseSeed(seedSpec);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: --seed: " << e.what() << "\n";
        return 1;
    }

    std::string source;
    if (!readWholeFile(assemblyFile, source))
    {
        std::cerr << "Error: cannot read '" << assemblyFile << "'.\n";
        return 1;
    }
    AssembledProgram program;
    try
    {
        program = assemble(assemblyFile);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Assembly failed: " << e.what() << "\n";
        return 1;
    }

    const reorder_report::ReorderOutcome outcome = reorder_report::runReorder(program, kind, seed);
    std::cout << "Reorder '" << assemblyFile << "' for " << reorder_report::vmName(kind) << "\n\n"
              << reorder_report::writeTextSummary(source, outcome);

    if (!htmlPath.empty())
    {
        if (!writeWholeFile(htmlPath, reorder_report::writeHtmlReport(source, outcome, assemblyFile)))
        {
            std::cerr << "Error: cannot write '" << htmlPath << "'.\n";
            return 1;
        }
        std::cout << "HTML report -> " << htmlPath << "\n";
    }

    if (!outPath.empty())
    {
        std::string reorderedSource;
        try
        {
            reorderedSource = reorder_report::writeReorderedSource(source, program, outcome.report);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: cannot write reordered assembly: " << e.what() << "\n";
            return 1;
        }
        if (!writeWholeFile(outPath, reorderedSource))
        {
            std::cerr << "Error: cannot write '" << outPath << "'.\n";
            return 1;
        }

        // The written file must assemble to exactly the words that were run.
        bool roundTripOk = false;
        try
        {
            std::istringstream stream(reorderedSource);
            roundTripOk = assemble(stream).text_buffer == outcome.report.words;
        }
        catch (const std::exception &)
        {
            roundTripOk = false;
        }
        std::cout << "Reordered assembly -> " << outPath << " (re-assembles to the run program: "
                  << (roundTripOk ? "yes" : "NO") << ")\n";
        if (!roundTripOk)
        {
            return 4;
        }
    }

    const bool valid = outcome.equivalent && !outcome.before.hitCap && !outcome.after.hitCap;
    return valid ? 0 : 4;
}

int RunReorderBenchCommand(int argc, char *argv[])
{
    std::string programsDir;
    std::string outDir = "reorder_results";
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc)
        {
            outDir = argv[++i];
        }
        else if (arg == "--help" || arg == "-h" || (!arg.empty() && arg[0] == '-'))
        {
            std::cerr << "Usage: " << argv[0]
                      << " reorder-bench <programs-dir> [--out <dir>]\n"
                         "  Filters every .s under <programs-dir> (header, I/M only, exit ecall last,\n"
                         "  no input syscalls, no stack, finishes within "
                      << bench_eval::kBenchCycleCap
                      << " cycles, near-duplicates removed),\n"
                         "  runs each usable program original vs reordered on rv5-h-f and rv5-h-nf,\n"
                         "  and writes filter.csv, results.csv, blocks.csv and summary.md to <dir>\n"
                         "  (default: reorder_results). Exit code 4 if any reordering is not\n"
                         "  equivalent.\n";
            return arg == "--help" || arg == "-h" ? 0 : 1;
        }
        else if (programsDir.empty())
        {
            programsDir = arg;
        }
        else
        {
            std::cerr << "Error: unexpected extra argument '" << arg << "'.\n";
            return 1;
        }
    }
    if (programsDir.empty() || !std::filesystem::is_directory(programsDir))
    {
        std::cerr << "Error: reorder-bench needs a directory of .s programs.\n";
        return 1;
    }

    const auto paths = bench_eval::findPrograms(programsDir);
    std::cerr << "Found " << paths.size() << " program(s) in " << programsDir << "\n";
    const bench_eval::BenchRun run =
        bench_eval::runBench(paths, [](const std::string &name) { std::cerr << "  " << name << "\n"; });

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const std::filesystem::path out(outDir);
    const bool written = writeWholeFile((out / "filter.csv").string(), bench_eval::filterCsv(run)) &&
                         writeWholeFile((out / "results.csv").string(), bench_eval::resultsCsv(run)) &&
                         writeWholeFile((out / "blocks.csv").string(), bench_eval::blocksCsv(run)) &&
                         writeWholeFile((out / "summary.md").string(), bench_eval::summaryMarkdown(run));
    if (!written)
    {
        std::cerr << "Error: cannot write results to '" << outDir << "'.\n";
        return 1;
    }
    std::cout << bench_eval::summaryMarkdown(run) << "\nWrote filter.csv, results.csv, blocks.csv, "
              << "summary.md to " << outDir << "\n";

    for (const bench_eval::ResultRow &r : run.results)
    {
        if (!r.outcome.equivalent)
        {
            return 4;
        }
    }
    return 0;
}

int RunReorderFuzzCommand(int argc, char *argv[])
{
    fuzz::FuzzOptions options;
    std::string programsDir = "bench/programs";
    size_t randomCount = fuzz::kDefaultRandomPrograms;
    std::string only;
    auto usage = [&]()
    {
        std::cerr << "Usage: " << argv[0]
                  << " reorder-fuzz [--count N] [--seed S] [--programs DIR] [--random R] [--only NAME]\n"
                     "  Runs each usable benchmark program in DIR (default bench/programs) and R\n"
                     "  generated random programs (default "
                  << fuzz::kDefaultRandomPrograms
                  << "), original vs reordered (optimizer + VM safety net),\n"
                     "  from N random starting states each (default "
                  << fuzz::kDefaultInputsPerProgram << ") on rv5-h-f and rv5-h-nf.\n"
                     "  --seed S   RNG seed (default "
                  << fuzz::kDefaultRngSeed
                  << "); every input is reproducible from it\n"
                     "  --only NAME  fuzz just the program with this name\n"
                     "  Exit code 4 if any run ends in a different state.\n";
    };
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        try
        {
            if (arg == "--count" && hasValue)
            {
                options.inputsPerProgram = static_cast<unsigned>(std::stoul(argv[++i]));
            }
            else if (arg == "--seed" && hasValue)
            {
                options.rngSeed = std::stoull(argv[++i], nullptr, 0);
            }
            else if (arg == "--programs" && hasValue)
            {
                programsDir = argv[++i];
            }
            else if (arg == "--random" && hasValue)
            {
                randomCount = std::stoul(argv[++i]);
            }
            else if (arg == "--only" && hasValue)
            {
                only = argv[++i];
            }
            else
            {
                usage();
                return arg == "--help" || arg == "-h" ? 0 : 1;
            }
        }
        catch (const std::exception &)
        {
            std::cerr << "Error: bad value for " << arg << ".\n";
            return 1;
        }
    }

    std::vector<fuzz::FuzzProgram> programs;
    if (!programsDir.empty() && std::filesystem::is_directory(programsDir))
    {
        programs = fuzz::benchmarkPrograms(programsDir);
    }
    else if (programsDir != "none")
    {
        std::cerr << "Error: '" << programsDir << "' is not a directory (use --programs none to skip).\n";
        return 1;
    }
    for (fuzz::FuzzProgram &p : fuzz::randomPrograms(randomCount, options.rngSeed))
    {
        programs.push_back(std::move(p));
    }
    if (!only.empty())
    {
        std::vector<fuzz::FuzzProgram> selected;
        for (fuzz::FuzzProgram &p : programs)
        {
            if (p.name == only)
            {
                selected.push_back(std::move(p));
            }
        }
        if (selected.empty())
        {
            std::cerr << "Error: no program named '" << only << "'.\n";
            return 1;
        }
        programs = std::move(selected);
    }

    const fuzz::FuzzReport report = fuzz::fuzzPrograms(programs, options);
    std::cout << fuzz::summaryText(report);
    return report.mismatches.empty() ? 0 : 4;
}
} // namespace Kites
