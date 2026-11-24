import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
MODELS = ("V1", "V3", "V4")
MODES = ("Witness", "Conditional", "MinIntervention")
NAMES = {"Witness": "固定见证抽样", "Conditional": "条件随机（最多 6 次）", "MinIntervention": "单次保留"}
RECIPES = {"g6-m3": "6 格／3 步", "g10-m4": "10 格／4 步", "g8-m2": "8 格／2 步", "g10-m3": "10 格／3 步"}


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def table(headers, entries):
    return "\n".join("| " + " | ".join(map(str, row)) + " |" for row in (headers, ["---"] * len(headers), *entries))


def pct(value):
    return f"{value:.2%}"


def interval(bounds, precision=4):
    return "[" + ", ".join(f"{value:+.{precision}f}" for value in bounds) + "]"


def main():
    v4 = load(HERE / "v4/results/summary.json")
    refill = load(HERE / "refill/full/analysis/controller-summary.json")
    changes = refill["changes_vs_exact_witness"]
    safety = refill["execution"]
    n = refill["cohort_size"]
    count = sum(safety[mode]["all"]["episodes"] for mode in MODES)
    assert n == 200 and count == 153600
    gap = v4["comparisons"]["V4_minus_V1"]
    v1_risk = [changes[mode]["V1"]["tier_changes"] for mode in MODES]

    model_table = table(("模型", "平均 Spearman ρ", "各非恒定单元中档位表现单调"), [
        (model, f"{item['macro_rho']:.4f}", f"{item['primary_cells_with_nondecreasing_tier_means']}/{item['eligible_primary_cells_for_tier_order']}")
        for model in MODELS for item in (v4["models"][model],)
    ])
    coverage = table(("配方", "V1 困难／普通／简单", "V3 困难／普通／简单", "V4 困难／普通／简单"), [
        (title, *("／".join(str(v4["models"][model]["coverage"][recipe][tier]) for tier in ("hard", "normal", "easy"))
                  for model in MODELS)) for recipe, title in RECIPES.items()
    ])
    controls = table(("补充方式", "V1 排名 ρ / 换档", "V3 排名 ρ / 换档", "V4 排名 ρ / 换档"), [
        (NAMES[mode], *(f"{value['macro_within_recipe_rho']:.3f} / {value['tier_changes']}/{n}"
                        for model in MODELS for value in (changes[mode][model],))) for mode in MODES
    ])
    proposal_table = table(("补充方式", "交换轮次", "自然提案", "不安全", "未知", "回退率", "实际换掉见证"), [
        (NAMES[mode], f"{data['total_turns']:,}", f"{data['proposal_attempts']:,}", f"{data['proposal_unsafe']:,}",
         f"{data['proposal_unknown']:,}", pct(data["witness_fallback_per_turn"]), pct(data["different_from_witness_per_turn"]))
        for mode in MODES[1:] for data in (safety[mode]["all"],)
    ])
    risk_table = table(("补充方式", "V1 换档", "V1 跨两档", "相对固定见证抽样的额外换档率", "额外换档率 95% 区间"), [
        (NAMES[mode], f"{item['tier_changes']}/{n}", item["two_tier_changes"], pct(excess["difference"]),
         "[" + ", ".join(pct(value) for value in excess["paired_board_bootstrap_ci95"]) + "]")
        for mode in MODES[1:] for item in (changes[mode]["V1"],) for excess in (item["excess_tier_shift_vs_witness_mc"],)
    ])
    stat = refill["verification"]["oracle_counters"]
    replay = refill["verification"]["witness_exact"]
    exact_error = max(replay["max_change_by_canonical_index"].values())
    extra_lines = ("为让三个控制器在同一状态使用同一选步规则，原证书中重复的状态采用最小节点编号。"
                   f"四代理的复算结果与归档结果的最大绝对差为 {exact_error:.2e}。"
                   "两种精确见证结果都已留档。")
    report = f"""# 三消 V4 与补充策略对照

**继续使用冻结的 V1 生产分档。V4 未达到替换门槛，改变补充策略会明显改变同一棋盘的模拟易度和档位。** V3 保留作研究对照。V2 暂停生产研究。现有 120 局的入池记录沿用原标准。

## 有效连锁贡献 V4

V4 用 `E[Kp] / Q` 表示有效连锁贡献。`Kp` 只统计每次交换中首波之后实际推进剩余任务的消除格数，累计到目标达成为止。它与余步、超额消除概率等权平均。原 100 局给出的新切点是 `0.406347` 和 `0.498239`。V1 的 `0.554`、`0.651` 及 V3 的已冻结切点按原版使用。

沿用前序按新种子认证的 200 局做 V4 首轮比较，四种配方各 50 局。200 份原证书再次通过独立 CLI 回放。这批棋盘已用于前序消融，V4 的切点只来自原 100 局。排序指标是在配方内衡量易度与 Greedy、Objective、Lookahead 余步和超额消除表现的关联，在 17 个非恒定单元上等权平均。

{model_table}

V4 对 V1 的差值为 **{gap['delta']:+.4f}**，配方内配对重采样 95% 区间为 **{interval(gap['ci95'])}**。未达到预设的 +0.02 提升门槛。8 格／2 步配方的点估计差为 −0.0503。有效贡献和超额达成率在 6 格／3 步与 10 格／4 步内的秩相关为 0.632 和 0.701，说明这两组的两个评分项存在较多重叠。

{coverage}

V4 有 50 局和 V1 档位不同，均为相邻换档。四个配方均有三档覆盖，但 6 格／3 步困难档只有 2 局，8 格／2 步简单档只有 1 局。有效连锁贡献可作单独诊断字段。

## 补充策略鲁棒性

在同一 200 个起始棋盘、相同四种选步策略上，每个组合跑 64 局，三个控制器共 **{count:,} 局**。固定见证使用原证书。条件随机最多抽 6 个自然配额提案，接受第一条证明安全的提案。单次保留仅抽一个，能证明安全就照用。两个随机控制器都在未获安全证明时回退到当前证书的见证。

对全体安全补充的完整枚举受计算预算限制。随机对照覆盖实际证明安全的子集。其中，条件随机在重试成功时按自然分布对该子集条件抽样。单次保留保留自然提案，拒绝质量集中回见证。在同一状态、同一已证安全集合上，两种实现都可达到相同的总变差下界；实际游玩访问的状态与回退次数可以不同。

{controls}

排名以配方内 Spearman ρ 等权平均。换档与原固定见证的精确分档比较，所有模型均沿用自己的固定切点。固定见证自身的 64 局抽样让 V1 有 {v1_risk[0]} 局换档，可作为抽样噪声参照。两种随机方式的换档分别为 **{v1_risk[1]}** 和 **{v1_risk[2]}** 局。

{risk_table}

{proposal_table}

所有 {count:,} 局都达到 Strict 基础目标。**{stat['natural_edges_replayed']:,} 条执行前的自然交换轨迹**逐条独立回放，另有 **{stat['certificates_verified']:,} 份新后继证书**独立验证。未决提案与已证不安全提案分开计数。求解缓存供同一棋盘的全部实验共享，计数用于复核执行，耗时不用于推断线上吞吐。

{extra_lines}

生产上应让**补充策略版本与易度版本一起绑定**。当前 V1 档位对应固定见证。切到随机补充前，应在该控制器下重新评分并验证档位。真人难度仍待玩家数据校准。

## 复现

`match3_v4/v4/results` 包含逐关分数、按配方和代理的指标及重叠分析。`match3_v4/refill/full` 保存全部回放统计、波次、审计台账和比较结果。`pilot` 是同种子、同口径的 32 局子集。归档携带 200 局棋盘、证书和固定的求解器提交。

```bash
git clone ./solver/match3-strict.bundle ./solver-checkout
bash match3_v4/reproduce.sh ./solver-checkout ./v4-replay
```
"""
    (HERE / "v4-refill-report.md").write_text(report, encoding="utf-8")


if __name__ == "__main__":
    main()
