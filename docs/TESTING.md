# Running Tests

Kites uses [GoogleTest](https://github.com/google/googletest) for unit testing, integrated with CMake/CTest. Two test targets are available:

- **`kites_test`** — plain test build, no instrumentation. Use this for everyday local testing.
- **`kites_test_cov`** — same test suite, built with coverage instrumentation (`--coverage`), plus a `coverage` target that generates an HTML report via [`gcovr`](https://gcovr.com/).

Both targets are produced from a single configure step; you choose which one to build/run afterward.

## Prerequisites

- CMake 3.24+
- Ninja
- A GCC- or Clang-style compiler (coverage instrumentation requires this — see [Notes](#notes))
- [`gcovr`](https://gcovr.com/) on your `PATH` (only required if configuring with `ENABLE_COVERAGE=ON`)

## Configure

Both test targets live under the same configure preset:

```sh
cmake --preset test
```

## Running tests (no coverage)

```sh
cmake --build --preset test-only
ctest --preset test-only
```

This builds and runs `kites_test` only — the fast path for everyday local test runs.

## Running tests (with coverage)

```sh
cmake --build --preset test-coverage
ctest --preset test-coverage
```

This builds and runs `kites_test_cov`, whose CTest test names are prefixed with `cov_` so they don't collide with the plain suite's test names.

### Generating the HTML coverage report

The `coverage` target runs the instrumented test suite via CTest and then invokes `gcovr` to produce an HTML report:

```sh
cmake --build build/coverage --target coverage
```

The report is written to `build/coverage/coverage/index.html`.

## Build options

Set via `-D<OPTION>=ON|OFF` at configure time, or in `CMakePresets.json`:

| Option              | Default | Description                                      |
|---------------------|---------|---------------------------------------------------|
| `DISABLE_GUI`        | `ON`    | Disable GUI components during tests               |
| `VM_DEBUG_PRINTS`     | `OFF`   | Enable debug prints in the VM during tests        |
| `ENABLE_COVERAGE`     | `OFF`   | Build the `kites_test_cov` target and `coverage` target |

## Notes

- Coverage instrumentation (`--coverage`) requires GCC or Clang; configuring with `ENABLE_COVERAGE=ON` on another compiler will fail with a clear error.
- If you're on Windows with a Qt-bundled MinGW toolchain, confirm it actually ships `libgcov.a` before relying on coverage builds — some trimmed-down distributions omit it. You can check with:
  ```sh
  g++ -print-file-name=libgcov.a
  ```
  A real path back means it's present; the bare filename echoed back means it isn't found.
## Code reordering: fuzzing and extra checks

The reorder library (`src/reorder/`) is covered by the `Reorder*` GoogleTest suites in the default
run. Large randomized runs go through the CLI instead, so the default `ctest` run stays fast.

### `kites-cli reorder-fuzz`

Runs each program, original and reordered (optimizer + VM safety net), on `rv5-h-f` and `rv5-h-nf`
from random starting registers and memory, and compares the final state (registers, the 64-word
data window, the stack window, and that the code was not overwritten). Every input is reproducible
from the RNG seed, which is printed with any failure.

```sh
# The original run: 54 benchmarks + 100 straight-line programs x 20 inputs x 2 pipelines
# (6,160 runs). Must report 0 mismatches.
kites-cli reorder-fuzz

# Structured programs: forward branches (all six) and jal, bounded countdown loops (some unrollable),
# every load/store size, la inside blocks, edge-case immediates and shift amounts, immediates that
# trigger the hazard unit's immediate-bits quirk, blocks of up to 64+ instructions.
kites-cli reorder-fuzz --generator v2 --programs none --random 500 --count 20
```

Options: `--count N` inputs per program, `--seed S`, `--programs DIR|none`, `--random R` generated
programs, `--only NAME`, `--generator v1|v2`, `--checks basic|all`.

Checks (`basic` is the default with `v1`, `all` with `v2`):

| Check | `basic` | `all` | Fails the run |
|---|:-:|:-:|:-:|
| Same final state on every random input | yes | yes | yes |
| Same final state on the declared input | yes | yes | yes |
| Never slower: inputs where the reordered program takes more cycles | yes | yes | no (listed with seeds) |
| Block invariants: same blocks, permutation inside each, terminator last, auipc in its slot | yes | yes | yes |
| Path replay cycles and `total_stalls_` equal the VM's (declared + 3 random inputs, before and after) | | yes | yes |
| Unroll + reorder keeps the final state on every input (and how many programs got unrolled) | | yes | yes |

The safety net only promises "not slower" on the declared input, so a slower random input is a
finding, not a failure. Exit code 4 means a correctness check failed.

### Optimizer by block size

```sh
kites-cli reorder-fuzz --optimizer-table --blocks 50
```

Prints a markdown table for random straight-line blocks of 8, 16, 24, 32, 48 and 64 instructions on
both pipelines: mean and max optimizer time, how many searches were proven optimal within the
200,000-position budget, and original/greedy/optimal predicted stalls. Times depend on the build type.

### Negative controls

`ReorderFuzzNegativeControl.*` injects a fault in test code and checks that the matching check
reports it: an auipc moved by one slot (block check), a store swapped with a later overlapping load
and a swapped WAR pair (final-state check), and the replay run with the Textbook hazard rule (replay
check). `ReorderFuzz.NegativeControlReportsMismatch` swaps a load with its use.

### Coverage of the reorder library on macOS (Apple clang)

`kites_test_cov` builds with Apple clang, but the `coverage` target needs `gcovr` and links a `gcov`
library that Apple clang does not ship. Without changing the project, in a separate build directory:

```sh
mkdir -p /tmp/fakegcov && echo "" | cc -x c -c - -o /tmp/fakegcov/empty.o && ar rcs /tmp/fakegcov/libgcov.a /tmp/fakegcov/empty.o
cmake -S . -B build/coverage-mac -G Ninja -DENABLE_TESTS=ON -DENABLE_COVERAGE=ON \
      -DGCOVR_EXECUTABLE=/usr/bin/true "-DCMAKE_EXE_LINKER_FLAGS=--coverage -L/tmp/fakegcov"
cmake --build build/coverage-mac --target kites_test_cov
cd test && ../build/coverage-mac/test/kites_test_cov
```

Then read the `.gcda` files with `xcrun llvm-cov gcov -b -c -o <object dir> <file.gcda>`. Header-only
code is instantiated in many objects, so merge per source line (covered if any object covered it).
`cli/reorder_command.cpp` is not part of `kites_test_cov`.
