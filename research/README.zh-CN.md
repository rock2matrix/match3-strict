# 三消严格可解性研究

[English](README.md) · [研究 Release 与完整资料](https://github.com/rock2matrix/match3-strict/releases/tag/research-2026-11-24)

本目录归档代理难度、离线关卡池、受控补充、ACR 和定向证明库扩充的代码与验收依据。实验绑定求解器提交 [71aac8a](https://github.com/rock2matrix/match3-strict/tree/71aac8aab2e3fae2fa798e075f0f7d52b1220754)。

## 已冻结的决定

现有 120 局继续使用固定安全见证和 `strict-random-tier-v1`。易度越大，代理表现越容易。V1 的困难／普通切点为 **0.554**，普通／简单切点为 **0.651**。Proof-Bank v1.1 按增量封存，ACR v1.1 保留为研究控制器。

| Proof-Bank v1.1 验收项 | 实测 |
| --- | --- |
| 新增安全续局 | 8,628 份，全部通过独立回放 |
| 同一批 92,760 条提案的已证安全比例 | 72.29% → 76.78% |
| Strict 基础目标 | 409,600／409,600，未认证补充执行 0 |
| ACR 见证回退 | 5.90%，高于 5% 门槛 |
| 同库固定 6 次的回退率 | 3.79% |
| ACR 相对同库固定 6 次 CPU 节省 | 冷况 4.37%，热况 3.86%，低于 10% 门槛 |

ACR v1.1 独立校准的困难／普通切点为 **0.690104**，普通／简单切点为 **0.744792**。独立 200 局验证得到困难 72、普通 51、简单 77。分数和切点绑定扩充库、补充策略及难度模型版本。

## 实验入口

| 阶段 | 源码和证据 |
| --- | --- |
| 自动玩家、V1 和补池 | [代理实验](match3_difficulty/) · [关卡池](match3_pool/) |
| 连锁消融 | [消融实验](match3_ablation/) |
| V4 有效连锁及补充策略对照 | [V4 及补充对照](match3_v4/) |
| ACR v1.0 与本地并发压测 | [ACR](match3_acr/) |
| Proof-Bank v1.1 扩库、验收与独立回放 | [Proof-Bank](match3_proofbank/) |

Git 保存源码、冻结参数和汇总证据。完整棋盘、证书、原始回放记录、负载记录与证明库增量可从研究 Release 下载。 [归档与 SHA-256 清单](artifacts.json) · [机器可读的项目状态](status.json)

在 WSL 或 Linux 下载 `match3-proof-bank-v11-wsl.zip`，再从仓库根目录执行以下命令。还原工具先校验归档摘要，再解压到新目录。

~~~bash
python3 research/restore_experiment.py ~/Downloads/match3-proof-bank-v11-wsl.zip ./proof-bank-evidence
cd ./proof-bank-evidence
sha256sum -c SHA256SUMS
git clone ./solver/match3-strict.bundle ./solver-checkout
bash match3_proofbank/reproduce.sh ./solver-checkout ./proof-bank-replay --smoke
~~~

去掉 `--smoke` 可执行完整对照和独立难度校准。实测 CPU 值来自 WSL 决策路径。真实服务容量和真人难度需要单独验收。[产品化集成路线图](productization.md)
