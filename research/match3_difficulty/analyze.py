#!/usr/bin/env python3
"""Summarize a Strict policy experiment with paired comparisons by board."""

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from itertools import combinations
from pathlib import Path
from statistics import fmean


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as source:
        return list(csv.DictReader(source))


def write_csv(path, rows):
    with path.open("w", encoding="utf-8-sig", newline="") as dest:
        writer = csv.DictWriter(dest, list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def quantile(sorted_values, p):
    index = (len(sorted_values) - 1) * p
    left = math.floor(index)
    right = math.ceil(index)
    return sorted_values[left] + (index - left) * (
        sorted_values[right] - sorted_values[left]
    )


def describe(values):
    values = sorted(values)
    if not values:
        return None
    return {
        "n": len(values),
        "mean": fmean(values),
        "min": values[0],
        "p10": quantile(values, 0.1),
        "median": quantile(values, 0.5),
        "p90": quantile(values, 0.9),
        "max": values[-1],
    }


def numeric(row, field):
    value = row.get(field, "")
    if value is None or not str(value).strip():
        return None
    try:
        result = float(value)
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def feature_distribution(rows, field):
    values = [numeric(row, field) for row in rows]
    return describe(value for value in values if value is not None)


def wilson(successes, trials):
    z = 1.959963984540054
    rate = successes / trials
    midpoint = (rate + z * z / (2 * trials)) / (1 + z * z / trials)
    half_width = (z / (1 + z * z / trials)) * math.sqrt(
        rate * (1 - rate) / trials + z * z / (4 * trials * trials)
    )
    return max(0.0, midpoint - half_width), min(1.0, midpoint + half_width)


def ranks(values):
    ordered = sorted(enumerate(values), key=lambda pair: pair[1])
    result = [0.0] * len(ordered)
    start = 0
    while start < len(ordered):
        end = start + 1
        while end < len(ordered) and ordered[end][1] == ordered[start][1]:
            end += 1
        rank = (start + 1 + end) / 2
        for index, _ in ordered[start:end]:
            result[index] = rank
        start = end
    return result


def spearman(first, second):
    if len(first) < 3 or len(set(first)) < 2 or len(set(second)) < 2:
        return None
    first = ranks(first)
    second = ranks(second)
    center = (len(first) + 1) / 2
    left = [value - center for value in first]
    right = [value - center for value in second]
    return math.fsum(a * b for a, b in zip(left, right)) / math.sqrt(
        math.fsum(a * a for a in left) * math.fsum(b * b for b in right)
    )


def table(headers, rows):
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join(["---"] * len(headers)) + " |",
    ]
    lines.extend("| " + " | ".join(map(str, row)) + " |" for row in rows)
    return "\n".join(lines)


def display(value, as_rate=False, signed=False):
    if value is None:
        return "—"
    if as_rate and signed:
        return f"{100 * value:+.2f} 个百分点"
    if as_rate:
        return f"{value:.2%}"
    return f"{value:+.3f}" if signed else f"{value:.3f}"


AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
OUTCOMES = {
    "challenge": ("challenge_count", "结算累计消除 ≥ 目标 + 3"),
    "early": ("early_count", "通关时至少剩一步"),
    "chain": ("chain_count", "任一回合至少两波消除"),
}
POLICY_METRICS = {
    "mean_clears": "累计消除",
    "mean_moves_left": "剩余步数",
    "mean_waves": "总消除波数",
    "mean_extra_waves": "额外连锁波数",
    "mean_action_evaluations": "动作评估次数",
    "mean_refill_probes": "补充探测次数",
}
FEATURES = {
    "legal_swaps": "首步合法交换数",
    "legal_swap_density": "首步合法交换密度",
    "color_entropy": "初盘归一化颜色熵",
    "d1_pressure": "D1 目标压力",
    "natural_mean_clears": "自然配额补充每步消除",
    "natural_goal_rate": "自然配额补充达标率",
    "natural_cap_rate": "自然配额补充遇批次上限率",
    "natural_depth_rate": "自然配额补充遇连锁深度上限率",
    "natural_dead_rate": "自然配额补充遇无合法交换率",
    "d2_entropy": "D2 首步动作价值归一化熵",
    "d2_best_gap": "D2 首步最优与次优预估价值差",
    "root_cascade_mean": "D3 首步自然消除波数均值",
    "root_cascade_variance": "D3 首步自然消除波数方差",
    "root_impact_ratio": "D3 首步影响格子占比",
    "root_safe_mass_lower": "D5 首步确定安全质量估计",
    "root_safe_mass_upper": "D5 首步可能安全质量估计",
    "root_witness_mass": "D5 首步单见证补充质量",
    "root_witness_tv": "D5 首步单见证补充 TV",
    "certificate_nodes": "Strict 证书节点数",
}


def check_inputs(candidates, levels, policies):
    if not candidates or not levels:
        raise ValueError("Expected nonempty candidate and level tables")
    for rows, label in ((candidates, "candidates"), (levels, "levels")):
        if len({row["id"] for row in rows}) != len(rows):
            raise ValueError(f"Duplicate id in {label}")
    level_ids = {row["id"] for row in levels}
    candidate_map = {row["id"]: row for row in candidates}
    if not level_ids <= candidate_map.keys():
        raise ValueError("Some simulated boards are missing from candidates.csv")
    for row in levels:
        candidate = candidate_map[row["id"]]
        for field in ("moves", "goal"):
            if field in candidate and int(candidate[field]) != int(row[field]):
                raise ValueError(f"Certification setup differs for {row['id']}/{field}")
        if "cert_verified" in candidate and candidate["cert_verified"] != "1":
            raise ValueError(f"No independently verified certificate for {row['id']}")
        lower = numeric(row, "root_safe_mass_lower")
        upper = numeric(row, "root_safe_mass_upper")
        if lower is not None and upper is not None and not 0 <= lower <= upper <= 1 + 1e-10:
            raise ValueError(f"Invalid certified safe refill mass bounds for {row['id']}")
    expected_pairs = {(board_id, agent) for board_id in level_ids for agent in AGENTS}
    actual_pairs = {(row["id"], row["agent"]) for row in policies}
    if expected_pairs != actual_pairs or len(policies) != len(expected_pairs):
        raise ValueError("Expected exactly one row for each (level, agent)")
    for row in policies:
        runs = int(row["runs"])
        if runs < 1 or int(row["wins"]) != runs:
            raise ValueError(f"Strict replay failed or no simulation ran for {row['id']}/{row['agent']}")
        for outcome, (count_field, _) in OUTCOMES.items():
            exact = float(row[f"exact_{outcome}"])
            count = int(row[count_field])
            if not (0 <= exact <= 1 + 1e-10 and 0 <= count <= runs):
                raise ValueError(f"Invalid rate for {row['id']}/{row['agent']}/{outcome}")


def interval_rows(policies):
    rows = []
    for policy in policies:
        runs = int(policy["runs"])
        for outcome, (count_field, _) in OUTCOMES.items():
            count = int(policy[count_field])
            lower, upper = wilson(count, runs)
            exact = min(1.0, float(policy[f"exact_{outcome}"]))
            rows.append({
                "id": policy["id"],
                "agent": policy["agent"],
                "outcome": outcome,
                "successes": count,
                "runs": runs,
                "mc_rate": count / runs,
                "wilson_95_lower": lower,
                "wilson_95_upper": upper,
                "exact_rate": exact,
                "absolute_mc_error": abs(count / runs - exact),
                "exact_in_interval": int(lower - 1e-12 <= exact <= upper + 1e-12),
            })
    return rows


def exact_outcome(policy, outcome):
    return min(1.0, float(policy[f"exact_{outcome}"]))


def threshold_sensitivity(policies):
    fields = {1: "exact_surplus_1", 3: "exact_challenge", 6: "exact_surplus_6"}
    optional_fields = (fields[1], fields[6])
    if not any(field in row for row in policies for field in optional_fields):
        return None
    for row in policies:
        rates = [numeric(row, field) for field in fields.values()]
        if any(rate is None for rate in rates):
            raise ValueError(f"Incomplete threshold sensitivity for {row['id']}/{row['agent']}")
        low, medium, high = rates
        if not (0 <= high <= medium + 1e-10 and medium <= low + 1e-10 and low <= 1 + 1e-10):
            raise ValueError(f"Inconsistent nested challenge rates for {row['id']}/{row['agent']}")
    result = {}
    for agent in AGENTS:
        rows = [row for row in policies if row["agent"] == agent]
        primary = [round(exact_outcome(row, "challenge"), 12) for row in rows]
        result[agent] = {}
        for extra, field in fields.items():
            values = [min(1.0, float(row[field])) for row in rows]
            result[agent][str(extra)] = describe(values) | {
                "at_zero": sum(rate < 1e-12 for rate in values),
                "at_one": sum(rate > 1 - 1e-12 for rate in values),
                "rank_correlation_with_3": spearman([round(rate, 12) for rate in values], primary),
            }
    return result


def compare_policies(seeds, by_policy, agent, other):
    paired = {}
    for outcome in OUTCOMES:
        first = [exact_outcome(by_policy[seed, agent], outcome) for seed in seeds]
        second = [exact_outcome(by_policy[seed, other], outcome) for seed in seeds]
        changes = [a - b for a, b in zip(first, second)]
        paired[outcome] = describe(changes) | {
            "greater": sum(change > 1e-12 for change in changes),
            "equal": sum(abs(change) <= 1e-12 for change in changes),
            "less": sum(change < -1e-12 for change in changes),
            "rank_correlation": spearman([round(rate, 12) for rate in first], [round(rate, 12) for rate in second]),
        }
    return paired


def summarize_refill_audit(audit, by_stratum):
    by_id = {row["id"]: row for row in audit}
    expected = {seed for seeds in by_stratum.values() for seed in seeds}
    if by_id.keys() != expected or len(by_id) != len(audit):
        raise ValueError("Expected one refill audit row for every selected level")
    raw_fields = (
        "samples", "shortcut_safe", "shortcut_unsafe", "solver_safe", "solver_unsafe",
        "unresolved", "depth_stops", "cap_stops", "solver_calls", "cache_hits",
    )
    draws_per_board = {int(row["samples"]) for row in audit}
    if len(draws_per_board) != 1:
        raise ValueError("Audit requires equal draws per board to aggregate boards with equal weight")
    groups = {name: [by_id[seed] for seed in seeds] for name, seeds in by_stratum.items()}
    groups["all"] = audit
    result = {}
    for name, rows in groups.items():
        group = {field: sum(int(row[field]) for row in rows) for field in raw_fields}
        safe = group["shortcut_safe"] + group["solver_safe"]
        unsafe = group["shortcut_unsafe"] + group["solver_unsafe"]
        samples = group["samples"]
        if safe + unsafe + group["unresolved"] != samples or samples <= 0:
            raise ValueError(f"Audit counts do not reconcile for {name}")
        lower = safe / samples
        upper = 1 - unsafe / samples
        # Jointly cover both endpoints within this fixed cohort, with alpha=.025 on each side.
        radius = math.sqrt(math.log(40.0) / (2 * samples))
        confidence = (max(0, lower - radius), min(1, upper + radius))
        result[name] = group | {
            "known_safe": safe,
            "known_unsafe": unsafe,
            "safe_mass_lower": lower,
            "safe_mass_upper": upper,
            "simultaneous_95_low": confidence[0],
            "simultaneous_95_high": confidence[1],
            "intervention_lower": 1 - upper,
            "intervention_upper": 1 - lower,
            "intervention_simultaneous_95_low": 1 - confidence[1],
            "intervention_simultaneous_95_high": 1 - confidence[0],
        }
    return result


def analyze(candidates, levels, policies, group_field, status_field, audit=None):
    check_inputs(candidates, levels, policies)
    by_level = {row["id"]: row for row in levels}
    by_policy = {(row["id"], row["agent"]): row for row in policies}
    selection = set(by_level)
    intervals = interval_rows(policies)
    cohort = {}
    by_stratum = defaultdict(list)
    for row in candidates:
        group = row[group_field] if group_field else "全部"
        entry = cohort.setdefault(group, {"candidates": 0, "statuses": Counter(), "selected": 0})
        entry["candidates"] += 1
        if status_field:
            entry["statuses"][row[status_field]] += 1
        if row["id"] in selection:
            entry["selected"] += 1
            by_stratum[group].append(row["id"])

    summary = {
        "candidates": len(candidates),
        "levels": len(levels),
        "cohort": cohort,
        "selected_goal_move_counts": [
            {"goal": int(goal), "moves": int(moves), "levels": count}
            for (moves, goal), count in sorted(Counter(
                (row["moves"], row["goal"]) for row in levels
            ).items(), key=lambda item: (int(item[0][0]), int(item[0][1])))
        ],
        "policies": {},
        "paired_policies": {},
        "outcome_by_stratum": {},
        "threshold_sensitivity": threshold_sensitivity(policies),
        "refill_audit": summarize_refill_audit(audit, by_stratum) if audit is not None else None,
        "features": {field: feature_distribution(levels, field) for field in FEATURES},
        "feature_challenge_spearman": {},
    }
    mass_bounds = [(numeric(row, "root_safe_mass_lower"), numeric(row, "root_safe_mass_upper")) for row in levels]
    classified = [(lo, hi) for lo, hi in mass_bounds if lo is not None and hi is not None]
    if classified:
        summary["root_refill"] = {
            "uncertain_share": describe(hi - lo for lo, hi in classified),
            "intervention_lower": describe(1 - hi for _, hi in classified),
            "intervention_upper": describe(1 - lo for lo, _ in classified),
        }
    for agent in AGENTS:
        agent_rows = [row for row in policies if row["agent"] == agent]
        entry = {
            "replays": sum(int(row["runs"]) for row in agent_rows),
            "wins": sum(int(row["wins"]) for row in agent_rows),
            "means": {field: feature_distribution(agent_rows, field) for field in POLICY_METRICS},
            "outcomes": {},
            "outcome_monte_carlo": {},
        }
        for outcome in OUTCOMES:
            sample = [row for row in intervals if row["agent"] == agent and row["outcome"] == outcome]
            rates = [row["exact_rate"] for row in sample]
            entry["outcomes"][outcome] = describe(rates) | {
                "at_zero": sum(rate < 1e-12 for rate in rates),
                "at_one": sum(rate > 1 - 1e-12 for rate in rates),
            }
            entry["outcome_monte_carlo"][outcome] = {
                "mean_absolute_error": fmean(row["absolute_mc_error"] for row in sample),
                "max_absolute_error": max(row["absolute_mc_error"] for row in sample),
                "exact_in_interval": sum(row["exact_in_interval"] for row in sample),
            }
        summary["policies"][agent] = entry

    for other, agent in combinations(AGENTS, 2):
        summary["paired_policies"][f"{agent} − {other}"] = compare_policies(by_level, by_policy, agent, other)

    for group, seeds in by_stratum.items():
        summary["outcome_by_stratum"][group] = {
            agent: {
                outcome: fmean(exact_outcome(by_policy[seed, agent], outcome) for seed in seeds)
                for outcome in OUTCOMES
            }
            for agent in AGENTS
        }

    for field in FEATURES:
        pair = [
            (value, exact_outcome(by_policy[seed, "Random"], "challenge"))
            for seed, row in by_level.items()
            if (value := numeric(row, field)) is not None
        ]
        first, second = zip(*pair) if pair else ((), ())
        summary["feature_challenge_spearman"][field] = spearman(first, second)

    return summary, intervals


def render_report(result):
    n = result["levels"]
    all_replays = sum(data["replays"] for data in result["policies"].values())
    all_wins = sum(data["wins"] for data in result["policies"].values())
    primary_rates = [result["policies"][agent]["outcomes"]["challenge"]["mean"] for agent in AGENTS]
    best_agent = AGENTS[primary_rates.index(max(primary_rates))]
    delta = max(primary_rates) - primary_rates[0]
    steps = Counter()
    for row in result["selected_goal_move_counts"]:
        steps[row["moves"]] += row["levels"]
    step_mix = '、'.join(f'{count} 局 {moves} 步' for moves, count in sorted(steps.items()))
    root_refill = result.get("root_refill")
    audit = result.get("refill_audit")
    refill_summary = ""
    if root_refill:
        refill_summary = (
            f"首批探测依赖证书里已有的状态和终止规则判断安全性，"
            f"按关卡等权有 {display(root_refill['uncertain_share']['mean'], True)} 的样本无法分类。"
        )
    audit_section = []
    if audit:
        overall = audit["all"]
        audit_section = [
            "## D5 首步安全补充复核",
            "",
            "追加探测每关 128 次，先均匀抽取合法交换，再抽取配额内自然补充。"
            "原证书或终止规则无法直接判定时，用 Strict 求解器继续验证后继状态，同时复核新得到的通关证书。"
            "预算内未得出结论的样本保留为未知。每关探测数相同，各关等权。",
            "",
            table(("分层", "样本数", "已证安全", "已证不安全", "未知", "样本安全质量上下界", "安全质量 95% 区间"), (
                ("全体" if name == "all" else name, f"{row['samples']:,}", f"{row['known_safe']:,}",
                 f"{row['known_unsafe']:,}", f"{row['unresolved']:,}",
                 f"{display(row['safe_mass_lower'], True)}–{display(row['safe_mass_upper'], True)}",
                 f"{display(row['simultaneous_95_low'], True)}–{display(row['simultaneous_95_high'], True)}")
                for name, row in audit.items()
            )),
            "",
            "安全质量上下界把未知样本全部计为不安全或安全，95% 区间进一步计入抽样误差。"
            "每行对两端使用 Hoeffding 单侧界，联合覆盖率至少为 95%。",
            "",
            f"全体首步自然补充的理论拒绝比例以 1 减去安全质量计算，样本上下界为 "
            f"{display(overall['intervention_lower'], True)}–{display(overall['intervention_upper'], True)}，"
            f"95% 区间为 {display(overall['intervention_simultaneous_95_low'], True)}"
            f"–{display(overall['intervention_simultaneous_95_high'], True)}。"
            "它衡量如果按自然分布提出补充，再拒绝不安全方案，会拒绝多少首步提案。"
            "单见证 TV 衡量实验所用的确定性补充策略与自然分布的差异。整局干预度仍待测量。",
            "",
        ]
    lookahead = result["policies"]["Lookahead"]["outcomes"]
    lookahead_pair = result["paired_policies"]["Lookahead − Random"]["challenge"]
    summary_of_ranking = (
        f"主挑战下，Lookahead 在 {lookahead['challenge']['at_one']} 个关卡上必达成，"
        f"在 {lookahead['challenge']['at_zero']} 个关卡上无法达成。"
        f"它在 {lookahead_pair['less']} 个关卡上的达成率低于 Random，"
        f"两者的关卡排名相关系数为 {display(lookahead_pair['rank_correlation'])}。"
        "单一代理和阈值会把很多关卡压到同一档，现阶段用它们划分关卡池档位容易遗漏差异。"
    )
    thresholds = result["threshold_sensitivity"]
    threshold_section = []
    if thresholds:
        random = thresholds["Random"]
        greedy = thresholds["Greedy"]
        changed_order = ""
        if random["1"]["mean"] > greedy["1"]["mean"] and random["3"]["mean"] < greedy["3"]["mean"]:
            changed_order = "Random 在 +1 的均值高于 Greedy，换成 +3 时顺序反转。"
        lookahead_high = thresholds["Lookahead"]["6"]
        endpoint_count = lookahead_high["at_zero"] + lookahead_high["at_one"]
        threshold_section = [
            "## 消除量阈值敏感性",
            "",
            table(("代理", "目标 + 1", "目标 + 3", "目标 + 6", "+6 达成率为 0／1 的关卡", "+6 与 +3 排名 Spearman ρ"), (
                (agent, *(display(values[str(extra)]["mean"], True) for extra in (1, 3, 6)),
                 f"{values['6']['at_zero']}／{values['6']['at_one']}", display(values['6']['rank_correlation_with_3']))
                for agent, values in thresholds.items()
            )),
            "",
            "三个阈值沿相同代理策略与证书路径计算。" + changed_order +
            f"Lookahead 在 +6 仍有 {endpoint_count} 个关卡落在 0% 或 100%，关卡间差异有限。",
            "",
        ]
    lines = [
        "# 三消难度量化 · 自动玩家实验",
        "",
        f"在 {result['candidates']} 个候选棋盘中选出 {n} 个经 Strict 认证的关卡。"
        f"四种代理共回放 {all_replays:,} 局，{all_wins:,} 局完成基础目标。"
        "用结算时累计消除至少比目标多 3 格作为主挑战。",
        "",
        f"{best_agent} 的主挑战平均达成率最高，为 {display(max(primary_rates), True)}。"
        f"与 Random 相差 {display(delta, True, True)}。这些数值按棋盘等权，从每个棋盘的证书策略图精确计算。",
        "",
        "## 样本构成",
        "",
        table(("分层", "候选", "认证状态计数", "入选"), (
            (group, item["candidates"], "、".join(f"{label} {count}" for label, count in sorted(item["statuses"].items())) or "—", item["selected"])
            for group, item in result["cohort"].items()
        )),
        "",
        f"入选棋盘的步数组成为 {step_mix}。固定分层和认证筛选会影响样本构成，以下均值只描述本批棋盘。",
        "",
        table(("步数", "目标消除量", "入选关卡"), (
            (row["moves"], row["goal"], row["levels"]) for row in result["selected_goal_move_counts"]
        )),
        "",
        "## 代理表现",
        "",
        table(("代理", "回放局数", "平均累计消除", "平均剩余步数", "平均额外连锁波数", "主挑战", "至少余一步", "至少一次两波消除"), (
            (agent, f"{row['replays']:,}",
             *(display(row["means"][field]["mean"]) for field in ("mean_clears", "mean_moves_left", "mean_extra_waves")),
             *(display(row["outcomes"][outcome]["mean"], True) for outcome in OUTCOMES))
            for agent, row in result["policies"].items()
        )),
        "",
        "代理决定交换动作，补充控制器沿同一份 Strict 证书执行对应见证。Random 随机选合法交换，Greedy 优先选首波消除最多的交换，"
        "Objective 按自然补充预估的目标进度和当回合达标机会行动，Lookahead 另外考虑连锁和下一盘面潜在收益。"
        "并列动作均匀选择。两种范围更广的策略用固定的配额补充探测估计价值。",
        "",
        table(("代理", "平均动作评估次数", "平均补充探测次数"), (
            (agent, *(display(row["means"][field]["mean"]) for field in ("mean_action_evaluations", "mean_refill_probes")))
            for agent, row in result["policies"].items()
        )),
        "",
        "动作评估和补充探测次数是代理决策工作量。",
        "",
        "## 关卡间差异",
        "",
        table(("代理", "主挑战 P10", "中位数", "P90", "达成率为 0 的局数", "达成率为 1 的局数"), (
            (agent, *(display(data[key], True) for key in ("p10", "median", "p90")), data["at_zero"], data["at_one"])
            for agent, row in result["policies"].items()
            for data in (row["outcomes"]["challenge"],)
        )),
        "",
        table(("配对比较", "主挑战率平均变化", "提高／相同／降低的局数", "关卡排名 Spearman ρ"), (
            (agent, display(data["mean"], True, True), f"{data['greater']}／{data['equal']}／{data['less']}", display(data["rank_correlation"]))
            for agent, row in result["paired_policies"].items()
            for data in (row["challenge"],)
        )),
        "",
        table(("分层主挑战率", *AGENTS), (
            (group, *(display(row[agent]["challenge"], True) for agent in AGENTS))
            for group, row in result["outcome_by_stratum"].items()
        )),
        "",
        summary_of_ranking,
        "",
        *threshold_section,
        "## 特征测量",
        "",
        "D1 以未做 Strict 安全选择的自然配额补充估算每步消除。D2 以自然补充下的预估回合消除量计算首步动作价值，"
        "归一化熵使用温度 1。D3 首步影响比例统计自然补充路径中的交换端点、各波匹配格和匹配格上方同列区域。"
        "D4 用累计消除、余步和连锁三个表现终点观察各代理。",
        "",
        table(("特征", "关卡数", "P10", "中位数", "P90", "与 Random 主挑战率的 Spearman ρ"), (
            (label, data["n"], *(display(data[key], field.endswith("_rate")) for key in ("p10", "median", "p90")),
             display(result["feature_challenge_spearman"][field]))
            for field, label in FEATURES.items()
            if (data := result["features"][field]) is not None
        )),
        "",
        "相关系数为在本批四个分层混合计算的描述值。分层、目标消除量和证书筛选都可能影响这些相关。",
        "",
        "D5 的首批探测对每个合法首步使用同样数量的配额内自然补充样本。"
        "单见证概率质量与 TV 对应当前证书指定的确定性补充序列，按合法首步等权。",
        "",
        refill_summary,
        "",
        *audit_section,
        "## 回放一致性",
        "",
        table(("代理", "主挑战率平均绝对误差", "最大绝对误差", f"精确率落入 95% Wilson 区间的关卡数／{n}"), (
            (agent, display(data["mean_absolute_error"], True), display(data["max_absolute_error"], True), data["exact_in_interval"])
            for agent, row in result["policies"].items()
            for data in (row["outcome_monte_carlo"]["challenge"],)
        )),
        "",
        "Wilson 区间衡量单关卡、单代理的蒙特卡洛误差。完整区间和另外两个挑战的精确率见 `board-policy-intervals.csv`。",
        "",
        "## 难度校准范围",
        "",
        "当前难度依据来自四种策略的额外挑战表现。模型要预测真人挑战达成率，还需要不同水平的玩家实际游玩，"
        "并用留出的玩家数据验证关卡难度排名。",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidates", type=Path)
    parser.add_argument("levels", type=Path)
    parser.add_argument("policies", type=Path)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--group-field", help="candidate field for report strata")
    parser.add_argument("--status-field", help="candidate field for certification status")
    parser.add_argument("--audit-levels", type=Path, help="refill audit CSV, default sibling of levels.csv if present")
    args = parser.parse_args()
    candidates = read_csv(args.candidates)
    levels = read_csv(args.levels)
    policies = read_csv(args.policies)
    fields = set(candidates[0]) if candidates else set()
    group_field = args.group_field or next((f for f in ("stratum", "group", "tier") if f in fields), None)
    status_field = args.status_field or next((f for f in ("verdict", "status", "certification") if f in fields), None)
    inferred_audit = args.levels.with_name("safe_mass_audit.csv")
    audit_path = args.audit_levels or (inferred_audit if inferred_audit.is_file() else None)
    audit = read_csv(audit_path) if audit_path else None
    result, intervals = analyze(candidates, levels, policies, group_field, status_field, audit)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "difficulty-summary.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8"
    )
    (args.out_dir / "difficulty-report.md").write_text(render_report(result), encoding="utf-8")
    write_csv(args.out_dir / "board-policy-intervals.csv", intervals)


if __name__ == "__main__":
    main()
