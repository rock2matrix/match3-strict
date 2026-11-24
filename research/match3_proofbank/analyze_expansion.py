import argparse
import csv
import json
import random
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "match3_acr"))
from analyze_acr import latency, metric_us, quantile, read_rows  # noqa: E402


MODES = ("Adaptive", "Fixed6")
RECIPES = ("g6-m3", "g10-m4", "g8-m2", "g10-m3")
COLORS = "01234"
HOLES = (3, 4, 5)
WORK_CAPS = {"candidate_checks": 512, "refill_nodes": 8192, "states": 128}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def read_game_summary(directory, protocol):
    keys, metadata = set(), {}
    summary_by_board = defaultdict(Counter)
    for row in read_rows(directory / "summary.csv"):
        id, agent, controller = row["id"], row["agent"], row["controller"]
        key = id, agent, controller
        assert key not in keys and controller in MODES and agent in protocol["benchmark"]["agents"], key
        keys.add(key)
        recipe = row["recipe"]
        assert metadata.setdefault(id, recipe) == recipe
        scores = {name: int(row[name]) for name in (
            "episodes", "wins", "total_turns", "executed_witness", "executed_proposal", "witness_fallbacks",
            "proposal_attempts", "proposal_safe", "proposal_unsafe", "proposal_unknown", "timeout_fallbacks",
            "cap_fallbacks", "planned_witness_turns", "early", "challenge", "chain", "productive_extra")}
        assert scores["episodes"] == scores["wins"] == protocol["benchmark"]["episodes_per_board_agent_arm"], key
        assert scores["executed_proposal"] + scores["executed_witness"] == scores["total_turns"], key
        assert scores["proposal_safe"] + scores["proposal_unsafe"] + scores["proposal_unknown"] == scores["proposal_attempts"], key
        assert scores["executed_proposal"] <= scores["proposal_safe"], key
        if "unverified_executions" in row:
            assert int(row["unverified_executions"]) == 0, key
        summary_by_board[id, controller].update(scores)
    assert len(metadata) == 200 and Counter(metadata.values()) == {recipe: 50 for recipe in RECIPES}
    assert keys == {(id, agent, mode) for id in metadata for agent in protocol["benchmark"]["agents"] for mode in MODES}
    return metadata, summary_by_board


def first_wave_color_tv(waves, size):
    n = sum(waves.values())
    if not n:
        return 0.0
    quota = [size // 5 + (i < size % 5) for i in range(5)]
    slots = [[0] * 5 for _ in range(size)]
    for string, count in waves.items():
        assert len(string) == size and [string.count(color) for color in COLORS] == quota, string
        for i, color in enumerate(string):
            slots[i][int(color)] += count
    return sum(abs(slot[c] / n - quota[c] / size) for slot in slots for c in range(5)) / (2 * size)


def summarize_one(directory, protocol):
    recipes, board = read_game_summary(directory, protocol)
    draws, turn_cpu, turn_wall = defaultdict(Counter), defaultdict(list), defaultdict(list)
    board_cpu, bucket, measured = defaultdict(list), defaultdict(Counter), defaultdict(Counter)
    max_counts = defaultdict(Counter)
    for row in read_rows(directory / "turns.csv"):
        id, mode = row["id"], row["controller"]
        recipe = recipes[id]
        duration, wall = metric_us(row, "cpu"), metric_us(row, "wall")
        turn_cpu[mode].append(duration)
        turn_wall[mode].append(wall)
        board_cpu[id, mode].append(duration)
        k = int(row["k"])
        assert 0 <= k <= 6
        assert mode != "Fixed6" or k == 6
        counts = {key: int(row[key]) for key in WORK_CAPS}
        for key, limit in WORK_CAPS.items():
            assert 0 <= counts[key] <= limit and counts["candidate_checks"] <= k
            max_counts[mode][key] = max(max_counts[mode][key], counts[key])
        safe, unsafe, unresolved = (int(row[key]) for key in ("proposal_safe", "proposal_unsafe", "proposal_unknown"))
        assert safe + unsafe + unresolved == counts["candidate_checks"]
        witness = int(row["used_witness"])
        if not witness:
            assert safe == 1 and not int(row["timeout"]), row
        row_metrics = {"turns": 1, "fallbacks": int(row["fallback"]) or int(row["planned_witness"]),
                       "attempts": counts["candidate_checks"], "safe": safe, "unsafe": unsafe, "unresolved": unresolved,
                       "planned_k": k, "admitted_natural": 1-witness}
        bucket[mode, recipe, row["bucket"]].update(row_metrics)
        measured[id, mode].update(row_metrics)
        waves = row["first_wave"]
        assert 3 <= len(waves) <= 12 and set(waves) <= set(COLORS)
        draws[mode, len(waves)][waves] += 1

    for key, value in board.items():
        assert value["total_turns"] == len(board_cpu[key]) == measured[key]["turns"], key
        assert value["witness_fallbacks"] == measured[key]["fallbacks"], key
        assert value["proposal_attempts"] == measured[key]["attempts"], key
        assert value["executed_proposal"] == measured[key]["admitted_natural"], key
        assert value["proposal_unknown"] == measured[key]["unresolved"], key

    raw = {}
    by_board = {}
    for mode in MODES:
        total = Counter()
        for id, recipe in recipes.items():
            value = board[id, mode]
            total.update(value)
            by_board[id, mode] = {"cpu": statistics.mean(board_cpu[id, mode]),
                                  "fallback": value["witness_fallbacks"]/value["total_turns"]}
        attempts, turns = total["proposal_attempts"], total["total_turns"]
        raw[mode] = {**total, "fallback_rate": total["witness_fallbacks"] / turns,
                     "proposal_unknown_rate": total["proposal_unknown"] / attempts,
                     "draws_per_turn": attempts / turns, "thread_cpu": latency(turn_cpu[mode]),
                     "wall": latency(turn_wall[mode]), "max_work_per_turn": dict(max_counts[mode])}
    color_rows = []
    all_holes = sorted({size for _, size in draws})
    for size in all_holes:
        color_rows.append({"first_wave_holes": size, **{
            f"{mode}_{suffix}": value for mode in MODES for suffix, value in (
                ("turns", sum(draws[mode, size].values())), ("position_color_tv", first_wave_color_tv(draws[mode, size], size)))}})
    denominator = sum(sum(draws["Fixed6", size].values()) for size in HOLES)
    tv = {mode: sum(row[f"{mode}_position_color_tv"] * row["Fixed6_turns"] for row in color_rows
                    if row["first_wave_holes"] in HOLES) / denominator for mode in MODES}
    return recipes, raw, by_board, bucket, color_rows, tv


def paired_interval(recipes, statistic, protocol):
    grouped = [[id for id in recipes if recipes[id] == recipe] for recipe in RECIPES]
    boot = protocol["benchmark"]["bootstrap"]
    rng = random.Random(boot["seed"])
    observed = [id for group in grouped for id in group]
    simulated = sorted(statistic([id for group in grouped for id in rng.choices(group, k=len(group))])
                       for _ in range(boot["replicates"]))
    return {"point": statistic(observed), "ci95": [quantile(simulated, .025), quantile(simulated, .975)]}


def read_audit(file, protocol):
    by_recipe_bucket = defaultdict(Counter)
    by_board = defaultdict(Counter)
    ids = {}
    for row in read_rows(file):
        id, recipe, bucket = row["id"], row["recipe"], row["bucket"]
        assert ids.setdefault(id, recipe) == recipe
        fields = ("draws", "base_safe", "base_unsafe", "base_unresolved", "bank_safe", "bank_unsafe",
                  "bank_unresolved", "unknown_to_safe", "unknown_to_unsafe", "unchanged")
        data = {name: int(row[name]) for name in fields}
        assert all(count >= 0 for count in data.values()) and data["draws"] > 0
        n = data["draws"]
        assert n == sum(data[name] for name in ("base_safe", "base_unsafe", "base_unresolved"))
        assert n == sum(data[name] for name in ("bank_safe", "bank_unsafe", "bank_unresolved"))
        assert data["base_safe"] + data["unknown_to_safe"] == data["bank_safe"]
        assert data["base_unsafe"] + data["unknown_to_unsafe"] == data["bank_unsafe"]
        assert data["bank_unresolved"] + data["unknown_to_unsafe"] + data["unknown_to_safe"] == data["base_unresolved"]
        assert data["unchanged"] + data["unknown_to_unsafe"] + data["unknown_to_safe"] == n
        by_recipe_bucket[recipe, bucket].update({"state_actions": 1, **data})
        by_board[id].update({"state_actions": 1, **data})

    assert Counter(ids.values()) == {recipe: 50 for recipe in RECIPES}
    total = Counter()
    for group in by_board.values():
        total.update(group)
    changes = {id: by_board[id]["unknown_to_safe"] / by_board[id]["draws"] for id in ids}
    metric = paired_interval(ids, lambda ids: sum(changes[id] for id in ids)/len(ids), protocol)
    result = {**total, "base_mass": total["base_safe"]/total["draws"], "expanded_mass": total["bank_safe"]/total["draws"],
              "equal_weight_board_delta_certified_mass": metric}
    rows = [{"recipe": recipe, "bucket": bucket, **value,
             "base_z_cert": value["base_safe"]/value["draws"], "expanded_z_cert": value["bank_safe"]/value["draws"]}
            for (recipe, bucket), value in sorted(by_recipe_bucket.items())]
    return result, rows


def reconcile_audit_pairs(base, expanded):
    fields = ("id", "recipe", "state_fp", "swap_a", "swap_b", "bucket", "draws", "base_safe", "base_unsafe", "base_unresolved")
    left = [tuple(row[key] for key in fields) for row in read_rows(base)]
    right = [tuple(row[key] for key in fields) for row in read_rows(expanded)]
    assert left == right, "the base and expanded arms did not audit the same exact state/action index"
    assert len(left) == len({key[:5] for key in left})


def read_proof_work(path, limit):
    main = ("profile_turns", "profile_proposals", "profile_unresolved", "stable_unresolved", "dedup_successors",
            "prioritized_missing", "base_solver_calls", "base_verified", "base_proof_nodes", "extra_solver_calls",
            "newly_verified", "certified_fail", "solver_unresolved", "proof_added_nodes", "extra_candidate_checks",
            "extra_refill_nodes", "extra_states", "base_cpu_ns", "profile_cpu_ns", "audit_cpu_ns", "expansion_cpu_ns")
    proof, ids = defaultdict(Counter), set()
    for row in read_rows(path):
        assert row["id"] not in ids and int(row["extra_limit"]) == limit
        ids.add(row["id"])
        stats = {key: int(row[key]) for key in main}
        assert stats["newly_verified"] + stats["certified_fail"] + stats["solver_unresolved"] == stats["extra_solver_calls"]
        assert stats["extra_solver_calls"] <= limit and stats["prioritized_missing"] <= stats["dedup_successors"]
        proof[row["recipe"]].update(stats)
    assert len(ids) == 200
    output = [{"recipe": recipe, **counts} for recipe, counts in proof.items()]
    total = Counter()
    for stats in proof.values():
        total.update(stats)
    return dict(total), output


def main():
    parser = argparse.ArgumentParser(description="Paired assessment of an expanded offline ACR proof bank")
    parser.add_argument("base_cold", type=Path)
    parser.add_argument("base_warm", type=Path)
    parser.add_argument("targeted_cold", type=Path)
    parser.add_argument("targeted_warm", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--protocol", type=Path, default=HERE / "protocol.json")
    args = parser.parse_args()
    protocol = read_json(args.protocol)
    result, buckets, colors = {}, [], []
    shared, boards = {}, {}
    for case in ("base_cold", "base_warm", "targeted_cold", "targeted_warm"):
        recipes, data, by_board, bucket, color_rows, tv = summarize_one(getattr(args, case), protocol)
        assert not shared or recipes == shared
        shared = recipes
        boards[case] = by_board
        result[case] = {"controllers": data, "standardized_first_wave_color_tv_h3_5": tv}
        buckets.extend({"case": case, "controller": mode, "recipe": recipe, "bucket": name, **value,
                        "fallback_rate": value["fallbacks"]/value["turns"],
                        "unknown_rate": value["unresolved"]/value["attempts"] if value["attempts"] else None}
                       for (mode, recipe, name), value in sorted(bucket.items()))
        colors.extend({"case": case, **row} for row in color_rows)

    gates = protocol["gates_for_targeted_adaptive"]
    for name in ("cold", "warm"):
        current = result[f"targeted_{name}"]
        paired = boards[f"targeted_{name}"]
        savings = paired_interval(shared, lambda ids: 1 - sum(paired[id, "Adaptive"]["cpu"] for id in ids) /
                                  sum(paired[id, "Fixed6"]["cpu"] for id in ids), protocol)
        fallback = paired_interval(shared, lambda ids: sum(paired[id, "Adaptive"]["fallback"] -
                                   paired[id, "Fixed6"]["fallback"] for id in ids)/len(ids), protocol)
        adapt = current["controllers"]["Adaptive"]
        tv = current["standardized_first_wave_color_tv_h3_5"]
        current["vs_expanded_fixed6"] = {"paired_cpu_savings": savings, "paired_fallback_change": fallback,
                                         "point_color_tv_delta": tv["Adaptive"] - tv["Fixed6"]}
        current["gates"] = {"strict": adapt["episodes"] == adapt["wins"],
                            "fallback_rate": adapt["fallback_rate"] <= gates["max_witness_reliance"],
                            "cpu_saving": savings["point"] >= gates["min_cpu_saving_vs_same_bank_fixed6"],
                            "cpu_p95": adapt["thread_cpu"]["p95_us"] <= gates["max_cpu_p95_ms"] * 1000,
                            "color_tv_tolerance": tv["Adaptive"] - tv["Fixed6"] <= gates["max_first_wave_color_tv_delta_with_sampling_tolerance"]}
        for mode in MODES:
            before = result[f"base_{name}"]["controllers"][mode]
            after = current["controllers"][mode]
            current.setdefault("vs_original_bank", {})[mode] = {
                "observed_fallback_change": after["fallback_rate"] - before["fallback_rate"],
                "observed_unknown_change": after["proposal_unknown_rate"] - before["proposal_unknown_rate"]}

    assert (args.targeted_warm / "matched-audit.csv").read_bytes() == (args.targeted_cold / "matched-audit.csv").read_bytes()
    reconcile_audit_pairs(args.base_cold / "matched-audit.csv", args.targeted_cold / "matched-audit.csv")
    result["matched_audit"], audit_rows = read_audit(args.targeted_warm / "matched-audit.csv", protocol)
    result["offline_proof_work"], work_rows = read_proof_work(args.targeted_cold / "expansion.csv",
                                                              protocol["evidence"]["extra_offline_solver_calls_per_board_cap"])
    result["promotion_screen_passed"] = all(all(result[case]["gates"].values()) for case in ("targeted_warm", "targeted_cold"))
    args.output.mkdir(exist_ok=True, parents=True)
    write_csv(args.output / "runtime-by-recipe-bucket.csv", buckets)
    write_csv(args.output / "color-tv-by-first-wave-size.csv", colors)
    write_csv(args.output / "matched-audit-by-recipe-bucket.csv", audit_rows)
    write_csv(args.output / "offline-expansion-by-recipe.csv", work_rows)
    (args.output / "benchmark-summary.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"matched": result["matched_audit"], "gates": {k: result[k]["gates"] for k in ("targeted_cold", "targeted_warm")}}, indent=2))


if __name__ == "__main__":
    main()
