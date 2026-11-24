# Match3 Strict Solver

[简体中文](README.zh-CN.md)

A C++20 library and command-line tool for verifying strictly solvable match-three boards. The solver models a game that controls refills. For every legal player swap, it must find at least one refill plan that reaches a state with the same guarantee. Each certified plan covers all cascade waves for that swap.

[MIT License](LICENSE)

## Research evidence

The research archive records certified level-pool generation, difficulty calibration, controlled refill and Proof-Bank v1.1. The current pool retains fixed certified witnesses and the frozen V1 standard. ACR v1.1 remains experimental. [Research index](research/README.md) · [Release archives](https://github.com/rock2matrix/match3-strict/releases/tag/research-2026-11-24)

## Rules and results

- The input is a stable rectangular board. A legal move exchanges two orthogonally adjacent pieces and forms a horizontal or vertical run of at least three pieces of one color.
- Every wave clears the union of matched cells, drops survivors, and fills the holes. Refill colors are divided as evenly as possible, with extra pieces assigned to the lower color indices. The default cap is 12 pieces per wave. The goal counts cleared pieces from all waves. The move counter decreases once per swap.
- A strategy succeeds when the goal has been met and the board has stabilized. Its certificate contains a witness refill plan for each legal swap at every reachable nonterminal state. Playing the board with this strategy requires the game to use the certified refills.

| Result | Meaning |
| --- | --- |
| `WIN_CERTIFIED` | A complete strategy covers every legal player swap, and each covered path reaches the goal within the move limit. |
| `FAIL_CERTIFIED` | The board cannot meet the strict condition under the game rules. An unfinished dead board is one example. |
| `UNKNOWN` | The search could not finish within its limits, or a winning strategy exceeded the certificate limits. |

The cost depends on the board, goal, move limit, and number of cascade branches. A result for one fixture gives no timing or coverage guarantee for other boards.

## Build and test

Use CMake 3.20 or later and a C++20 compiler. The project has no third-party dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Command line

The [sample board](examples/board-7x9.txt) has seven columns and nine rows. A board file contains one row per line, with space-separated color indices from 0 through 4, and can be up to 8 KiB. Request a certificate for clearing six pieces within two moves, then verify it against the same input.

```sh
./build/match3-solve examples/board-7x9.txt 6 2 --certificate board.m3cert
./build/match3-solve verify examples/board-7x9.txt 6 2 board.m3cert
```

With a multi-configuration Windows build, use `.\build\Release\match3-solve.exe` in PowerShell. Run `match3-solve --help` for usage. The solve command prints the result on the first line, followed by `key=value` counters such as `candidate_checks`, `refill_nodes`, `states`, `legal_swaps`, and `safe_swaps`. Only `WIN_CERTIFIED` can write a strategy certificate. Verification binds the certificate to the supplied board, goal, move limit, and rules, then independently replays its reachable strategy. It prints `PASS` with exit code 0 or `REJECTED` with exit code 2. Invalid input and usage or I/O errors also return a nonzero exit code.

In the ZIP bundled with executables, run `bin/windows-x64/match3-solve.exe` on Windows or `./bin/linux-x64/match3-solve` on Linux x64 with glibc 2.38 or later. Pass the same arguments as above. Source checkouts can build the executable using the CMake commands above.

## Use from C++

Include `<match3_strict/solver.hpp>`. Fill a `match3_strict::Problem` with the board in row-major order, the goal, and the move limit. Call `solve(problem)` to get a verdict, work counters, and the certificate for a certified win. Call `verify_certificate(problem, result.certificate)` to validate it against the same problem. If you set a custom cascade limit for solving, pass it to verification as the third argument. The standalone build includes the `match3-example` executable. [C++ example](examples/basic.cpp)

The library accepts 5–16 rows and columns, 2–8 colors, goals up to 1,000,000, and up to 20 moves. Start with a board that has no matches. Invalid inputs or budgets throw `std::invalid_argument` from `solve`. The default `SearchBudget` allows 256,000 candidate checks, 2,880,000 refill nodes, 6,000 expanded states, and nine cascade waves per move. You can pass a custom budget to `solve`.

The certificate uses `M3CERT 1` text and supports up to 32 MiB and 200,000 nodes. When a winning strategy exceeds those limits, `work.certificate_limit` indicates the cause of the `UNKNOWN` verdict. Cutoff counters record pause or cutoff events during search.

Install the package to a local prefix.

```sh
cmake --install build --config Release --prefix install
```

In a consuming project's `CMakeLists.txt`, import and link the library.

```cmake
find_package(match3_strict CONFIG REQUIRED)
add_executable(my_solver main.cpp)
target_link_libraries(my_solver PRIVATE match3_strict::match3_strict)
```

Set `CMAKE_PREFIX_PATH` to the absolute path of the install prefix when configuring the consuming project. A project that keeps this source tree as a subdirectory can use `add_subdirectory(match3-strict)` and link the same target. Enable `MATCH3_STRICT_BUILD_CLI`, `MATCH3_STRICT_BUILD_EXAMPLE`, or `MATCH3_STRICT_BUILD_TESTS` to build those extras from a parent project.

## Continuous integration and delivery

Pushes and pull requests to `main` trigger Release builds on Ubuntu 24.04 and Windows. Each job runs CTest, solves and independently verifies the example certificate through the CLI, and builds a separate CMake consumer against the installed library. [GitHub Actions workflow](.github/workflows/ci.yml)

Push a version tag such as `v0.1.0`, or run the workflow from `main` with `release_tag` set to the version. The tag must match the CMake project version. After both jobs pass, the workflow delivers these files to the corresponding GitHub Release.

- `match3-strict-linux-x64.zip` and `match3-strict-windows-x64.zip` include the CLI, documentation, license, and 7-column × 9-row example board.
- `match3-strict-source.zip` contains the project sources for building the library and tools.

Existing release assets are preserved on rerun. To use a CLI ZIP, extract it and run `./match3-solve` on Linux or `.\match3-solve.exe` on Windows from the extracted directory. The Linux build uses the system glibc on the runner.

## Read a strategy certificate

The text starts with `M3CERT 1`. The next line contains the maximum refill size, cascade limit, root node index, and number of nodes. Below it, each node has the following records in order.

| Record | Fields |
| --- | --- |
| Node header | Rows, columns, color count, remaining goal, remaining moves, number of edges |
| Board | One color index per cell in row-major order |
| Edge header | Two cell indices to swap, child node index, number of refill waves |
| Refill wave | Number of tiles, then the color indices in fill order |

Cell and node indices start at zero. Each edge has one refill record for each wave. Fill holes one column at a time from left to right, and from top to bottom within each column. Start at the root. After each legal player swap, apply the edge with those cell indices and continue at its child. A node with zero remaining goal completes the strategy.
