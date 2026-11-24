# ACR 的 M1 与 M3 复跑

`acr_benchmark.cpp` 在归档求解器版本 `71aac8aab2e3fae2fa798e075f0f7d52b1220754` 上比较固定见证、固定 1／3／6 次提案和 Adaptive。每个棋盘分别从独立种子流建立证书目录、审计安全质量和采样对局。三步共同使用同一套棋盘与规则。

## 离线证明和安全质量

每局首先独立验证原始证书。离线对每个根交换抽样 12 次，再让四个代理各探索 16 局，每交换最多提案 3 次。每个棋盘最多调用 400 次后继求解，每次上限为 16,000 候选、45,000 补充节点、600 状态和 9 层连消。新的终局轨迹或者后继证书必须通过独立校验，才能进入离线目录。各控制器共用冻结后只读的证明目录。

审计记录至多 160 个不同状态／交换。使用与建库和实测对局不同的随机域，每个样本位置抽样 12 次。只读判定分已知安全、已知不安全和未决。分桶考虑剩余步数、第一波能否完成目标和剩余目标压力。样本不足 24 的桶回退到同一棋盘的汇总估计。`mass.csv` 同时给出实际计数和供规划器使用的样本量。分桶下界是这组已收集数据的保守预测，不能解释为对单个未见状态的保障。

## 在线执行和统计范围

运行时只生成 quota 自然轨迹、查询离线证明并逐条重放采纳的候选。独立的自然采样流与动作随机流在各控制器间对齐。每回合候选／补充格写入／稳定后继检查上限分别为 512／8192／128。候选之间与采纳前检查线程 CPU 的 2 ms 阈值。这个阈值是协作式停止条件，另记录拒绝、未决和超时。Adaptive 选择零提案时也计入见证依赖率。

`warm` 使用每个控制器独立、起始为空的后继查询缓存，在同棋盘的代理和对局间复用。`cold` 禁用该缓存。两者都能读同一离线证明目录，且 OS 缓存状态未强制清空。离线目录在进程内建立，是可供工程验证的原型；跨进程制品分发和服务接入另需实现。

## 命令

从资料根目录运行。先使用固定源码构建原始求解库，再组装实验程序。输出目录需要尚未存在。

```bash
cmake -S "$SOLVER" -B "$BUILD/strict" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/strict" -j 8
python3 match3_acr/build_benchmark.py "$SOLVER" "$BUILD/strict/libmatch3_strict.a" "$BUILD/acr"
DATASET=match3_ablation/fresh/dataset
"$BUILD/acr/acr-benchmark" "$DATASET" "$RUN/warm" 64 50 8 202611245 warm all
"$BUILD/acr/acr-benchmark" "$DATASET" "$RUN/cold" 64 50 8 202611245 cold all
python3 match3_acr/validate_benchmark.py "$RUN/warm" "$RUN/cold" "$RUN/validation.json"
```

`summary.csv` 聚合棋盘、代理和控制器。`episodes.csv` 为逐局目标推进明细。`turns.csv` 记录逐交换预算、提案类别、耗时、第一波颜色和见证轨迹的自然概率。`offline-bank.csv` 给出离线证明开销、采纳证书数量。`witness-exact.csv` 用原证书直接计算基准，与采样结果分别保存。
