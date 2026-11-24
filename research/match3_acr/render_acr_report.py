import csv
import hashlib
import json
import statistics
from pathlib import Path


HERE = Path(__file__).resolve().parent
WORK = HERE.parent
ASSISTED = ("Witness", "Fixed1", "Fixed3", "Fixed6", "Adaptive")
NAMES = dict(zip(ASSISTED, ("固定见证", "固定 1 次", "固定 3 次", "固定 6 次", "ACR")))


def from_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def from_csv(path):
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def table(head, contents):
    return "\n".join("| " + " | ".join(map(str, row)) + " |" for row in (head, ["---"] * len(head), *contents))


def rate(value):
    return f"{value:.2%}"


def bounds(entry):
    return "[" + ", ".join(rate(value) for value in entry["ci95"]) + "]"


def span(rows, field, scale=1, places=2):
    values = [float(row[field]) / scale for row in rows]
    return f"{min(values):,.{places}f}–{max(values):,.{places}f}"


def main():
    result_dir = HERE / "results"
    m3 = from_json(result_dir / "analysis/m3-summary.json")
    m1 = [row for row in from_csv(result_dir / "analysis/m1-mass-by-recipe-bucket.csv") if row["bucket"] == "all"]
    alpha = from_json(result_dir / "m3-validation.json")
    load = from_csv(HERE / "load/load-summary.csv")
    load_check = from_json(HERE / "load/validation.json")
    m5_lock = result_dir / "m5-analysis/acr-standard-lock.json"
    m5 = from_json(m5_lock)
    m5_results = from_json(result_dir / "m5-analysis/validation-summary.json")

    plan = from_json(HERE / "experiment-plan.json")
    assert sha(WORK / "match3_pool/frozen-standard.json") == plan["production_v1_sha256"]
    assert sha(m5_lock) == m5_results["lock_sha256"]
    assert not m3["adoption_screen_supported_both_cache_modes"]
    assert load_check["completion_checks_passed"] == load_check["admitted"]
    assert m5_results["validation_n"] == 200

    sample_counts = {field: sum(int(row[field]) for row in m1) for field in ("slots", "draws", "safe", "unsafe", "unresolved")}
    reference = from_csv(result_dir / "analysis/m3-first-wave-distribution.csv")
    projected = {row["controller"]: row for row in reference
                 if row["cache_mode"] == "warm" and row["first_wave_holes"] == "3"}
    warm = m3["warm"]["controllers"]
    cold = m3["cold"]["controllers"]
    cold_effect, warm_effect = (m3[mode]["paired_acr_vs_fixed6"] for mode in ("cold", "warm"))
    m1_table = table(("配方", "提案", "已证安全", "已证不安全", "未决", "已证安全占比"), [
        (row["recipe"], f"{int(row['draws']):,}", f"{int(row['safe']):,}", f"{int(row['unsafe']):,}",
         f"{int(row['unresolved']):,}", rate(float(row["safe_point"]))) for row in m1])
    exp_table = table(("控制器", "热缓存平均提案／回合", "热缓存回退", "热缓存线程 CPU P95／P99", "冷况 CPU P99"), [
        (NAMES[key], f"{data['draws_per_turn']:.3f}", "—" if key == "Witness" else rate(data["fallback_rate"]),
         f"{data['thread_cpu']['p95_us']:.2f}／{data['thread_cpu']['p99_us']:.2f} µs",
         f"{cold[key]['thread_cpu']['p99_us']:.2f} µs") for key in ASSISTED for data in (warm[key],)])
    m4_rows = {name: [row for row in load if row["controller"] == "ACR" and row["cache"] == "Warm" and row["scenario"] == name]
               for name in ("eight_worker", "eight_worker_burst")}
    compare_load = table(("8 工作线程热况", "CPU P99", "排队 P99", "计划到达至完成 P99", "每轮拒绝"), [
        ("40,000 次／秒", span(rows, "cpu_p99_us") + " µs", span(rows, "queue_p99_us") + " µs", span(rows, "due_to_finish_p99_us") + " µs", span(rows, "rejected", places=0))
        for rows in (m4_rows["eight_worker"],)
    ] + [
        ("320,000 次／秒压测", span(rows, "cpu_p99_us") + " µs", span(rows, "queue_p99_us", 1000) + " ms",
         span(rows, "due_to_finish_p99_us", 1000) + " ms", span(rows, "rejected", places=0))
        for rows in (m4_rows["eight_worker_burst"],)
    ])
    tier_table = table(("配方", "困难", "普通", "简单"), [
        (name, entry["hard"], entry["normal"], entry["easy"]) for name, entry in m5_results["by_recipe"].items()])
    wave = {key: float(projected[key]["first_wave_order_empirical_tv"]) for key in ("Witness", "Fixed6", "Adaptive")}
    confidence = [min(float(row["matched_uniform_mc_tv"]) for row in projected.values()),
                  max(float(row["matched_uniform_mc_tv"]) for row in projected.values())]
    m3_warm_proofs = alpha["warm"]["offline"]
    report = f"""# ACR v1.0 工程执行报告

**WSL 已完成 M1–M5。本轮保留现行固定见证和 V1 生产分档。** ACR 在验证过的续局闭包内正常运行，随机补充的首波分布更接近自然配额。相比固定 6 次，冷／热况均未达到预设的 10% CPU 节省门槛，ACR 继续作为研究候选。

## M1–M2 安全统计与控制器

从 200 局收集 **{sample_counts['slots']:,} 个状态与交换组合**。离线证明库冻结后独立抽取 **{sample_counts['draws']:,}** 次自然提案，共 {sample_counts['safe']:,} 次已证安全、{sample_counts['unsafe']:,} 次已证不安全、{sample_counts['unresolved']:,} 次未决。统计保留状态桶、单侧 95% Wilson 下界和稀疏桶所用的回退统计来源。

{m1_table}

运行时动态选择 0–6 次。固定支持集下，提高次数会降低见证回退，但 `K≥1` 时完整轨迹相对自然分布的理论 TV 为 `1−z`。`one` 落实为尝试耗尽后使用已验证见证。线上原型只做冻结证明库查询及自然轨迹回放，在验证通过并且预算仍可用时采用。选步后的非终局状态都带有完整的后续证明。

离线使用 {m3_warm_proofs['solver_calls']:,} 次求解调用，新验证 {m3_warm_proofs['certificates_verified']:,} 份证书。在线运行全部走已证明的支持集或见证。校验器逐回合核对次数、证明路径与动作规则。

## M3 冷况与独享缓存对照

200 局、四代理、每组 64 局。每种缓存条件四个正式控制器共 204,800 局，另加固定 3 次对照 51,200 局。两种缓存条件合计 **512,000 局**；缓存条件复用相同种子以便比较。所有回放完成 Strict 基础目标。冷况关闭在线查找 memo；独享缓存况在每个控制器自己的回放中积累 memo，控制器之间不共享。

{exp_table}

按棋盘等权配对，ACR 相对固定 6 的 CPU 节省是 **{rate(cold_effect['cpu_saving_fraction']['point'])} 冷况、{rate(warm_effect['cpu_saving_fraction']['point'])} 热况**，配方内配对 95% 区间分别为 {bounds(cold_effect['cpu_saving_fraction'])}、{bounds(warm_effect['cpu_saving_fraction'])}。配对回退增加约 {rate(warm_effect['fallback_delta']['point'])}，在 2 个百分点容差内；ACR 的实测总体回退 {rate(warm['Adaptive']['fallback_rate'])} 高于 5% 预测目标。

热况中，首波恰有 3 格的补充顺序 TV 为固定见证 {wave['Witness']:.4f}、固定 6 次 {wave['Fixed6']:.4f}、ACR {wave['Adaptive']:.4f}，匹配样本量的均匀参考 TV 为 {confidence[0]:.4f}–{confidence[1]:.4f}。逐位置颜色 TV、首波顺序 KL 与全轨迹按组估计的理论值分别保存在统计文件。全轨迹理论值受支持集估计和预算触发影响。

独立审计的未决占比是 {rate(sample_counts['unresolved']/sample_counts['draws'])}，ACR 实际尝试中是 {rate(warm['Adaptive']['proposal_unknown']/warm['Adaptive']['proposal_attempts'])}。两组对状态和动作的采样方式不同，Wilson 桶下界只用于分配尝试预算。后续优化优先看实际访问状态的证明库覆盖和每次查表回放成本。

## M4 本地并发与预算

16 局、167 个固定的已认证状态动作组合，比较 1、4、8 工作线程以及两个高压档。共投递 **{load_check['offered']:,}** 次，完成 **{load_check['admitted']:,}** 次，队列满拒绝 {load_check['rejected']:,} 次。全部完成请求都通过安全校验。离散预算超限为零；测得 {load_check['cpu_over_2ms']} 次请求超过 2 ms CPU 软预算，{load_check['deadline_capped']} 次触发内部限时回退。

{compare_load}

40,000 次／秒档每轮投递 12,000 次。320,000 档每轮投递 24,000 次。时延是 WSL 进程内的排队与回放测量，未计入真实网络、存储、部署和请求调度。时间检查在候选的前后进行，2 ms 用于约束继续提案。

## M5 控制器绑定的难度校准

生产最终选择仍为固定见证，因此其 V1 切点和已入池 120 局继续沿用。另为 ACR 候选用原 100 局独立锁定切点 **{m5['cuts'][0]:.6f}、{m5['cuts'][1]:.6f}**。易度使用 ACR 下 Random 的余步、超额消除和连锁概率的平均值。在与 M3 和校准都无种子重叠的另一批 200 局上，得到困难 **{m5_results['counts']['hard']}**、普通 **{m5_results['counts']['normal']}**、简单 **{m5_results['counts']['easy']}**。

{tier_table}

直接沿用见证的切点会换档 {m5_results['changed_if_reusing_witness_cuts']} 局，按 ACR 重校准后仍有 {m5_results['changed_after_controller_specific_calibration']} 局与旧档不同。跨代理排序相关约 {m5_results['acr_rank_macro']:.3f}，属于自动代理表现验证。ACR 校准制品与控制器绑定，保留为研究版。

## 复跑资料

`engineering-plan.md` 保存规则与验收标准。`results/run-binding.json` 绑定版本、源码、数据及结果。`results/analysis`、`results/m5-analysis` 和 `load` 含汇总与原始记录。解包后从根目录执行下面命令。

```bash
git clone ./solver/match3-strict.bundle ./solver-checkout
bash match3_acr/reproduce.sh ./solver-checkout ./acr-replay
```
"""
    (HERE / "ACR-results.md").write_text(report, encoding="utf-8")

    identity_sources = (
        HERE / "acr_controller.cpp", HERE / "safety_mass_estimator.cpp", HERE / "acr_benchmark.cpp", HERE / "acr_load.cpp",
        HERE / "acr_controller.hpp", HERE / "build_benchmark.py", HERE / "experiment-plan.json",
        WORK / "match3_difficulty/simulate.cpp", WORK / "match3_v4/refill/refill_oracle.hpp",
        WORK / "match3_v4/refill/refill_experiment.cpp", WORK / "match3_pool/frozen-standard.json", m5_lock,
        result_dir / "analysis/m3-summary.json", result_dir / "m5-analysis/validation-summary.json", HERE / "load/validation.json",
    )
    bindings = {"solver_commit": plan["solver_commit"], "controller_profile": "acr-balanced-offline-catalog-v1-study",
                "production_model": "strict-random-tier-v1", "production_refill": "fixed-witness",
                "research_model": m5["version"], "file_sha256": {path.relative_to(WORK).as_posix(): sha(path) for path in identity_sources}}
    (result_dir / "run-binding.json").write_text(json.dumps(bindings, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
