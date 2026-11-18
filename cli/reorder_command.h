/**
 * @file reorder_command.h
 * @brief `kites-cli reorder`: schedule a program for a hazard pipeline and compare it to the original.
 */
#pragma once

namespace Kites
{
// argv[1] is "reorder". Returns the process exit code.
int RunReorderCommand(int argc, char *argv[]);

// argv[1] is "reorder-bench": validity filter + batch run over a directory of benchmark programs,
// writing filter.csv, results.csv, blocks.csv and summary.md.
int RunReorderBenchCommand(int argc, char *argv[]);
} // namespace Kites
