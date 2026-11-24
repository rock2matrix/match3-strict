import argparse
import csv
import hashlib
import json
import math
import random
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "match3_ablation"))
from ablation import association, label, rank_vector  # noqa: E402


CONTROLLERS = ("Witness", "Fixed1", "Fixed3", "Fixed6", "Adaptive")
RECIPES = ("g6-m3", "g10-m4", "g8-m2", "g10-m3")
RULE_CAPS = {"candidate_checks": 512, "refill_nodes": 8192, "states": 128}


def read_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        yield from csv.DictReader(stream)


def csv_result(path, records):
    if records:
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(records[0]))
            writer.writeheader()
            writer.writerows(records)


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, result):
    path.write_text(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def mode_name(value):
    aliases = {"ACR": "Adaptive", "MinIntervention": "Fixed1", "Conditional": "Fixed6", "ConditionalRandom": "Fixed6"}
    return aliases.get(value, value)


def quantile(sorted_values, p):
    return sorted_values[max(0, math.ceil(len(sorted_values) * p) - 1)]


def latency(values):
    ordered = sorted(values)
    return {"mean_us": statistics.mean(ordered), "p50_us": quantile(ordered, .50),
            "p95_us": quantile(ordered, .95), "p99_us": quantile(ordered, .99), "max_us": ordered[-1]}


def metric_us(row, prefix):
    for name in (f"{prefix}_us", f"{prefix}_ns", f"{prefix}_ms", f"thread_{prefix}_us", f"turn_{prefix}_us"):
        if name in row:
            factor = 0.001 if name.endswith("_ns") else 1000 if name.endswith("_ms") else 1
            return float(row[name]) * factor
    raise KeyError(f"No {prefix} time column in turns table: {list(row)}")


def series_fields(summary):
    total = Counter()
    rows = 0
    for row in summary:
        rows += 1
        metrics = ("episodes", "wins", "early", "challenge", "chain", "total_turns", "executed_witness", "executed_proposal",
                   "witness_fallbacks", "proposal_attempts", "proposal_safe", "proposal_unsafe", "proposal_unknown",
                   "candidate_checks", "refill_nodes", "states", "cache_hits", "timeout_fallbacks",
                   "cap_fallbacks", "planned_witness_turns")
        total.update({field: int(row[field]) for field in metrics})
    assert rows and total["wins"] == total["episodes"]
    assert total["executed_witness"] + total["executed_proposal"] == total["total_turns"]
    assert total["proposal_safe"] + total["proposal_unsafe"] + total["proposal_unknown"] == total["proposal_attempts"]
    assert total["proposal_safe"] >= total["executed_proposal"]
    turns, n = total["total_turns"], total["episodes"]
    return {**total, "fallback_rate": total["witness_fallbacks"] / turns,
            "draws_per_turn": total["proposal_attempts"] / turns,
            "natural_executed_per_turn": total["executed_proposal"] / turns,
            "early_rate": total["early"] / n, "challenge_rate": total["challenge"] / n, "chain_rate": total["chain"] / n}


def exact_witness_drift(records, reference):
    results = {}
    v1 = (0.554, 0.651)
    for mode in CONTROLLERS:
        rhos = []
        changes, two_tiers = 0, 0
        n = 0
        for recipe in RECIPES:
            points = [row for row in records if mode_name(row["controller"]) == mode and row["recipe"] == recipe and row["agent"] == "Random"]
            observed = [statistics.mean(int(row[key]) / int(row["episodes"]) for key in ("early", "challenge", "chain")) for row in points]
            before = [reference[row["id"]] for row in points]
            assert len(before) == len({row["id"] for row in points})
            exact = [float(row["V1_ease"]) for row in before]
            rhos.append(association(rank_vector(observed), rank_vector(exact)))
            for row, ease in zip(before, observed):
                mode_label = label(ease, v1)
                changes += mode_label != row["V1_tier"]
                two_tiers += {mode_label, row["V1_tier"]} == {"hard", "easy"}
            n += len(points)
        results[mode] = {"n": n, "macro_rank_rho_vs_witness": statistics.mean(rhos), "tier_changes": changes,
                         "two_tier_changes": two_tiers}
    return results


def bootstrap_cost_and_fallback(by_board, fallback_by_board, recipes):
    grouped = [[id for id, key in recipes.items() if key == recipe] for recipe in RECIPES]
    mean_cpu = {key: statistics.mean(values) for key, values in by_board.items()}
    per_board_delta = {id: fallback_by_board[id, "Adaptive"] - fallback_by_board[id, "Fixed6"] for id in recipes}

    def compare(ids):
        saving = 1 - sum(mean_cpu[id, "Adaptive"] for id in ids) / sum(mean_cpu[id, "Fixed6"] for id in ids)
        delta = sum(per_board_delta[id] for id in ids) / len(ids)
        return saving, delta

    original = compare([id for group in grouped for id in group])
    rng = random.Random(202611247)
    options = [compare([id for group in grouped for id in rng.choices(group, k=len(group))]) for _ in range(2000)]
    return {key: {"point": original[i], "ci95": [quantile(ordered, 0.025), quantile(ordered, 0.975)]}
            for i, key in enumerate(("cpu_saving_fraction", "fallback_delta"))
            for ordered in (sorted(point[i] for point in options),)}


def finite_uniform_errors(n, support, simulated_rng):
    tv, kl = [], []
    for _ in range(12):
        counts = Counter(simulated_rng.randrange(support) for _ in range(n))
        tv.append(1 - sum(min(amount / n, 1 / support) for amount in counts.values()))
        kl.append(sum((amount / n) * math.log((amount / n) * support) for amount in counts.values()))
    return statistics.mean(tv), statistics.mean(kl)


def summarize_distribution(traces):
    result = []
    rng = random.Random(202611248)
    for (mode, holes), counts in sorted(traces.items()):
        n = counts.total()
        quota = [(holes // 5) + (i < holes % 5) for i in range(5)]
        support = math.factorial(holes) // math.prod(math.factorial(x) for x in quota)
        cells = [Counter() for _ in range(holes)]
        for order, count in counts.items():
            assert [order.count(str(i)) for i in range(5)] == quota, order
            for pos, color in enumerate(order):
                cells[pos][int(color)] += count
        color_tv = statistics.mean(sum(abs(cell[color] / n - quota[color] / holes) for color in range(5)) / 2 for cell in cells)
        order_tv = 1 - sum(min(amount / n, 1 / support) for amount in counts.values())
        order_kl = sum((amount / n) * math.log((amount / n) * support) for amount in counts.values())
        background_tv, background_kl = finite_uniform_errors(n, support, rng)
        result.append({"controller": mode, "first_wave_holes": holes, "turns": n, "quota_permutations": support,
                       "mean_position_color_tv": color_tv, "first_wave_order_empirical_tv": order_tv,
                       "first_wave_order_empirical_kl_nats": order_kl, "matched_uniform_mc_tv": background_tv,
                       "matched_uniform_mc_kl_nats": background_kl})
    return result


def planned_trace_law(k, z, witness_mass):
    # Under a fixed accepted support and no budget clipping. The plug-in z can
    # miss the witness in the audit sample; the known witness mass is a floor.
    assert 0 < witness_mass <= 1 and 0 <= z <= 1
    if k == 0:
        return 1 - witness_mass, -math.log(witness_mass), 1.0
    z = max(z, witness_mass)
    fallback = (1 - z) ** k
    alpha = sum((1-z) ** i for i in range(k))
    q_witness = alpha * witness_mass + fallback
    kl = max(0.0, (alpha * (z - witness_mass) * math.log(alpha) + q_witness * math.log(q_witness / witness_mass)))
    return 1-z, kl, fallback


def one_sided_wilson_lower(successes, n):
    if not n:
        return 0.0
    z = 1.6448536269514722
    p = successes / n
    scaled = z*z/n
    return max(0.0, (p+scaled/2-z*math.sqrt((p*(1-p)+scaled/4)/n))/(1+scaled))


def mass_statistics(cold, warm):
    source = cold / "mass.csv"
    assert hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256((warm / "mass.csv").read_bytes()).digest()
    raw = list(read_rows(source))
    assert len({(row["id"], row["bucket"]) for row in raw}) == len(raw)
    grouped = defaultdict(list)
    for row in raw:
        counts = [int(row[field]) for field in ("safe", "unsafe", "unresolved")]
        n = sum(counts)
        assert int(row["slots"]) * 12 == n
        lo, point, hi = (float(row[key]) for key in ("z_lower", "z_point", "z_upper"))
        assert 0 <= lo <= point <= hi <= 1
        forecast_n = int(row["forecast_samples"])
        if row["borrowed"] == "0":
            assert forecast_n == n and abs(lo - one_sided_wilson_lower(counts[0], n)) < 1e-10
        grouped[row["recipe"], row["bucket"]].append(row)
    result = []
    for (recipe, bucket), selection in sorted(grouped.items()):
        counts = {name: sum(int(row[name]) for row in selection) for name in ("slots", "safe", "unsafe", "unresolved")}
        n = counts["safe"] + counts["unsafe"] + counts["unresolved"]
        result.append({"recipe": recipe, "bucket": bucket, **counts, "draws": n,
                       "safe_point": counts["safe"] / n if n else None,
                       "wilson95_one_sided_lower": one_sided_wilson_lower(counts["safe"], n) if n else None,
                       "possible_safe95_one_sided_upper": 1-one_sided_wilson_lower(counts["unsafe"], n) if n else None,
                       "boards_using_pooled_fallback": sum(int(row["borrowed"]) for row in selection)})
    return result


def analyzed_mode(location, plan, reference):
    records = list(read_rows(location / "summary.csv"))
    grouped = defaultdict(list)
    per_board = defaultdict(list)
    recipes = {}
    for row in records:
        mode = mode_name(row["controller"])
        grouped[mode].append(row)
        per_board[row["id"], mode].append(row)
        recipes[row["id"]] = row["recipe"]
    assert set(grouped) == set(CONTROLLERS) and len(recipes) == 200
    assert len(records) == len(recipes) * 4 * len(CONTROLLERS)
    assert len({(row["id"], row["agent"], mode_name(row["controller"])) for row in records}) == len(records)
    assert {int(row["episodes"]) for row in records} == {plan["m3"]["episodes_per_level_agent_controller_cache"]}
    cpu, wall, by_board, traces, ks = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(Counter), defaultdict(Counter)
    laws = defaultdict(Counter)
    for row in read_rows(location / "turns.csv"):
        mode, id = mode_name(row["controller"]), row["id"]
        c, w = metric_us(row, "cpu"), metric_us(row, "wall")
        assert c >= 0 and w >= 0
        cpu[mode].append(c)
        wall[mode].append(w)
        by_board[id, mode].append(c)
        for name, cap in RULE_CAPS.items():
            if name in row:
                assert 0 <= int(row[name]) <= cap, row
        wave = row.get("first_wave", "")
        if wave:
            assert set(wave) <= set("01234") and 3 <= len(wave) <= 12, wave
            traces[mode, len(wave)][wave] += 1
        for field in ("k", "planned_k", "attempts_planned", "k_planned"):
            if field in row:
                k = int(row[field])
                assert 0 <= k <= 6
                ks[mode][k] += 1
                break
        if "witness_p0" in row:
            tv, kl, fallback = planned_trace_law(k, float(row["z_point"]), float(row["witness_p0"]))
            laws[mode].update({"n": 1, "tv": tv, "kl": kl, "fallback": fallback})

    result = {}
    for mode in CONTROLLERS:
        values = series_fields(grouped[mode])
        assert values["total_turns"] == len(cpu[mode]), mode
        result[mode] = {**values, "thread_cpu": latency(cpu[mode]), "wall": latency(wall[mode]), "planned_k": dict(sorted(ks[mode].items()))}
        if laws[mode]["n"]:
            result[mode]["group_forecasted_trajectory_law"] = {k: laws[mode][k] / laws[mode]["n"] for k in ("tv", "kl", "fallback")}
    fallback_by_board = {key: sum(int(row["witness_fallbacks"]) for row in values) /
                         sum(int(row["total_turns"]) for row in values) for key, values in per_board.items()}
    paired = bootstrap_cost_and_fallback(by_board, fallback_by_board, recipes)
    settings = plan["adoption_screen"]
    gates = {"strict": all(row["episodes"] == row["wins"] for row in result.values()),
             "fallback": paired["fallback_delta"]["point"] <= settings["max_extra_fallback_rate"],
             "cpu_saving": paired["cpu_saving_fraction"]["point"] >= settings["minimum_cpu_saving"],
             "cpu_p95": result["Adaptive"]["thread_cpu"]["p95_us"] <= 1000 * settings["max_sampled_turn_p95_thread_cpu_ms_screen"]}
    return {"controllers": result, "paired_acr_vs_fixed6": paired, "engineering_gates": gates,
            "frozen_witness_drift": exact_witness_drift(records, reference)}, summarize_distribution(traces)


def main():
    parser = argparse.ArgumentParser(description="Validate and compare paired WSL ACR controller benchmark outputs")
    parser.add_argument("cold", type=Path)
    parser.add_argument("warm", type=Path)
    parser.add_argument("witness_levels", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--plan", type=Path, default=ROOT / "experiment-plan.json")
    args = parser.parse_args()
    reference = {row["id"]: row for row in read_rows(args.witness_levels)}
    plan = load_json(args.plan)
    args.output.mkdir(parents=True, exist_ok=True)
    csv_result(args.output / "m1-mass-by-recipe-bucket.csv", mass_statistics(args.cold, args.warm))
    result = {}
    distributions = []
    for name in ("cold", "warm"):
        result[name], bins = analyzed_mode(getattr(args, name), plan, reference)
        distributions.extend({"cache_mode": name, **row} for row in bins)
    result["adoption_screen_supported_both_cache_modes"] = all(all(result[name]["engineering_gates"].values()) for name in ("cold", "warm"))
    write_json(args.output / "m3-summary.json", result)
    csv_result(args.output / "m3-first-wave-distribution.csv", distributions)
    for name in ("cold", "warm"):
        print(name, result[name]["paired_acr_vs_fixed6"], result[name]["engineering_gates"])


if __name__ == "__main__":
    main()
