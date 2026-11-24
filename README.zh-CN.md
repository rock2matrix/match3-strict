# 三消严格可解性求解器

[English](README.md)

这是一个用于验证三消棋盘严格可解性的 C++20 库和命令行工具。模型允许游戏控制补充。对玩家的每一种合法交换，求解器都需要找到至少一套补充方案，使后继状态也满足同样的保证。一套完整方案覆盖该次交换触发的所有连锁波次。

[MIT 许可证](LICENSE)

## 规则与判定

- 输入为稳定的矩形棋盘。合法操作是横向或纵向交换两个相邻方块，使三个或更多同色方块连成一线。
- 每波先清除全部匹配格，再让其余方块下落并补满空位。补充颜色尽量均分，余数依次给编号较小的颜色。默认每波最多补 12 格。所有连锁波次的消除都计入目标，每次交换只消耗一步。
- 达成目标且棋盘稳定后才算通关。证书为每个可达的非终止状态中的每一种合法交换保存一套补充方案。游戏沿证书进行时，需要使用这些经过验证的补充。

| 判定 | 含义 |
| --- | --- |
| `WIN_CERTIFIED` | 证书覆盖所有合法交换，覆盖的每条路径都能在步数限制内完成目标。 |
| `FAIL_CERTIFIED` | 在当前规则下无法满足严格可解性。目标未达成且棋盘无合法操作是其中一种情况。 |
| `UNKNOWN` | 达到搜索限制仍未得到确定判定，或找到的获胜策略超出证书限制。 |

搜索开销取决于棋盘、目标、步数和连锁分支数。单个样例的结果无法预测其他棋盘的耗时或认证率。

## 构建与测试

需要 CMake 3.20 或更高版本及支持 C++20 的编译器。项目没有第三方依赖。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## 命令行

[示例棋盘](examples/board-7x9.txt)为 7 列 × 9 行。棋盘文件每行对应一行棋盘，用空格分隔颜色编号，范围为 0 到 4。文件上限为 8 KiB。求解两步内消除六格的目标，并用相同输入独立验证证书。

```sh
./build/match3-solve examples/board-7x9.txt 6 2 --certificate board.m3cert
./build/match3-solve verify examples/board-7x9.txt 6 2 board.m3cert
```

在 Windows 多配置构建中，在 PowerShell 使用 `.\build\Release\match3-solve.exe`。可用 `match3-solve --help` 查看用法。求解命令在首行输出判定，随后以 `key=value` 形式列出搜索计数，例如 `candidate_checks`、`refill_nodes`、`states`、`legal_swaps` 和 `safe_swaps`。仅 `WIN_CERTIFIED` 可以写出策略证书。验证命令将证书与输入棋盘、目标、步数和规则绑定，再独立回放可达的策略分支。通过时输出 `PASS` 并返回 0。拒绝时输出 `REJECTED` 并返回 2。输入、用法或文件读写有误时也会返回非零退出码。

如果下载的是带可执行文件的 ZIP，Windows x64 下可运行 `bin/windows-x64/match3-solve.exe`，Linux x64 下可运行 `./bin/linux-x64/match3-solve`，后者需要 glibc 2.38 或更高版本。参数与上面相同。源码可通过上面的 CMake 命令构建这些工具。

## C++ 接入

包含 `<match3_strict/solver.hpp>`，按行展平填入 `match3_strict::Problem` 的棋盘，并设置目标和步数。调用 `solve(problem)` 可获得判定、搜索计数和获胜时的证书。调用 `verify_certificate(problem, result.certificate)` 可对照同一输入验证证书。如果求解时使用了自定义的连锁波数限制，验证时将该限制作为第三个参数传入。独立构建会生成 `match3-example` 可执行文件。 [C++ 示例](examples/basic.cpp)

库支持 5–16 行、5–16 列、2–8 种颜色，目标最高为 1,000,000，步数最多为 20。请使用无匹配的稳定棋盘。输入或预算无效时，`solve` 抛出 `std::invalid_argument`。默认 `SearchBudget` 允许检查 256,000 个补充候选、访问 2,880,000 个补充节点、展开 6,000 个状态，每次交换最多搜索九波连锁。调用 `solve` 时可以传入自定义预算。

证书使用 `M3CERT 1` 文本格式，上限为 32 MiB 和 200,000 个节点。获胜策略超出上限时，返回 `UNKNOWN`，并通过 `work.certificate_limit` 表明原因。截断计数记录搜索中发生的暂停或截断事件。

先将库安装到本地目录。

```sh
cmake --install build --config Release --prefix install
```

在调用项目的 `CMakeLists.txt` 中导入和链接库。

```cmake
find_package(match3_strict CONFIG REQUIRED)
add_executable(my_solver main.cpp)
target_link_libraries(my_solver PRIVATE match3_strict::match3_strict)
```

配置调用项目时，将 `CMAKE_PREFIX_PATH` 指向安装目录的绝对路径。也可以将源码作为子目录，通过 `add_subdirectory(match3-strict)` 加入项目，再链接同一目标。在父项目中启用 `MATCH3_STRICT_BUILD_CLI`、`MATCH3_STRICT_BUILD_EXAMPLE` 或 `MATCH3_STRICT_BUILD_TESTS`，即可构建相应的工具、示例或测试。

## 持续集成与交付

向 `main` 推送或提交目标为 `main` 的拉取请求时，Ubuntu 24.04 和 Windows 分别运行 Release 构建。每个任务执行 CTest，用 CLI 求解示例并独立验证证书，再使用已安装的库编译独立的 CMake 调用项目。 [GitHub Actions 工作流](.github/workflows/ci.yml)

推送 `v0.1.0` 这类版本标签，或在 `main` 手动运行工作流并填入 `release_tag`，就能触发发布。标签必须与 CMake 项目版本一致。两项任务均通过后，工作流将以下文件交付到对应的 GitHub Release。

- `match3-strict-linux-x64.zip` 和 `match3-strict-windows-x64.zip` 包含 CLI、文档、许可证和 7 列 × 9 行的示例棋盘。
- `match3-strict-source.zip` 包含构建库与工具的项目源码。

重新运行工作流会保留已有的 Release 附件。解压 CLI 包后，在解压目录中运行 Linux 版的 `./match3-solve` 或 Windows 版的 `.\match3-solve.exe`。Linux 版使用构建环境的系统 glibc。

## 读取策略证书

第一行是 `M3CERT 1`。第二行依次记录单波补充上限、连锁波数上限、根节点编号和节点数。随后每个节点按下表排列。

| 记录 | 字段 |
| --- | --- |
| 节点头 | 行数、列数、颜色数、剩余消除目标、剩余步数、交换边数 |
| 棋盘 | 按行展平的各格颜色编号 |
| 交换边头 | 待交换的两个格子编号、后继节点编号、补充波数 |
| 单波补充 | 宝石数量以及按填充顺序排列的颜色编号 |

格子和节点均从 0 编号。每条交换边包含该次交换所有波次的补充记录。补充时从左到右按列处理，同一列内从上到下填充空格。运行时从根节点出发，玩家每做一次合法交换，就执行对应边的补充并切到后继节点。剩余消除目标归零即通关。
