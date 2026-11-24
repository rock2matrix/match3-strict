import argparse
import csv
import hashlib
import json
import random
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "match3_ablation"))
from ablation import association, label, rank_vector  # noqa: E402


RECIPES = ("g6-m3", "g10-m4", "g8-m2", "g10-m3")
AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
MODES = ("Witness", "Conditional", "MinIntervention")
MODELS = ("V1", "V3", "V4")
ENDPOINTS = ("early", "challenge")
TIERS = ("hard", "normal", "easy")


def rows(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        result = list(csv.DictReader(stream))
    for row in result:
        if row.get("controller") == "ConditionalRandom":
            row["controller"] = "Conditional"
    return result


def write_csv(path, records):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def index_unique(records, columns):
    indexed = {}
    for row in records:
        key = tuple(row[col] for col in columns)
        if key in indexed:
            raise ValueError(f"Duplicate row {key}")
        indexed[key] = row
    return indexed


def rho(left, right):
    a = rank_vector([round(value, 12) for value in left])
    b = rank_vector([round(value, 12) for value in right])
    return None if a is None or b is None else association(a, b)


def probability_ranks(x, y):
    a = rank_vector([round(value, 12) for value in x])
    b = rank_vector([round(value, 12) for value in y])
    return None if b is None else association(a, b)


def scores(metric):
    first, second = metric["early"], metric["challenge"]
    return {"V1": statistics.mean((first, second, metric["chain"])),
            "V3": statistics.mean((first, second)),
            "V4": statistics.mean((first, second, metric["productive"])),}


def exact_probabilities(row):
    return {"early": float(row["exact_early"]), "challenge": float(row["exact_challenge"]),
            "chain": float(row["exact_chain"]),
            "productive": float(row["exact_mean_productive_extra_tiles"]) / int(row["goal"])}


def estimated_probabilities(row):
    n = int(row["episodes"])
    return {"early": int(row["early"]) / n, "challenge": int(row["challenge"]) / n,
            "chain": int(row["chain"]) / n, "productive": int(row["productive_extra"]) / (n * int(row["goal"]))}


def summarize_execution(all_rows, mode, plan):
    fields = ("episodes", "wins", "total_turns", "executed_witness", "executed_proposal", "witness_fallbacks",
              "proposal_attempts", "proposal_safe", "proposal_unsafe", "proposal_unknown", "proposal_cap", "proposal_depth",
              "proposal_same_witness")
    totals = {k: sum(int(row[k]) for row in all_rows) for k in fields}
    if totals["wins"] != totals["episodes"]:
        raise ValueError("At least one episode missed the strict goal")
    if totals["executed_witness"] + totals["executed_proposal"] != totals["total_turns"]:
        raise ValueError("Executed turns did not reconcile")
    if totals["proposal_safe"] + totals["proposal_unsafe"] + totals["proposal_unknown"] != totals["proposal_attempts"]:
        raise ValueError("Natural proposals did not reconcile")
    if totals["proposal_safe"] != totals["executed_proposal"]:
        raise ValueError("Accepted proposals did not reconcile")
    if mode != "Witness" and totals["executed_witness"] != totals["witness_fallbacks"]:
        raise ValueError("Witness fallbacks did not reconcile")
    retry = plan["controllers"][mode]["attempts_per_turn"]
    minimum = totals["total_turns"] if retry else 0
    if not minimum <= totals["proposal_attempts"] <= retry * totals["total_turns"]:
        raise ValueError("Refill attempts did not respect the controller rule")
    totals["witness_fallback_per_turn"] = totals["witness_fallbacks"] / totals["total_turns"]
    totals["certified_natural_per_turn"] = totals["executed_proposal"] / totals["total_turns"]
    totals["different_from_witness_per_turn"] = (totals["executed_proposal"] - totals["proposal_same_witness"]) / totals["total_turns"]
    totals["unresolved_per_proposal"] = (totals["proposal_unknown"] / totals["proposal_attempts"]
                                         if totals["proposal_attempts"] else 0.0)
    return totals


def check_episode_ledger(path, index):
    counts = defaultdict(Counter)
    seen = set()
    for row in rows(path):
        key = (row["id"], row["agent"], row["controller"])
        identifier = (*key, row["run"])
        if identifier in seen:
            raise ValueError(f"Repeated episode {identifier}")
        seen.add(identifier)
        record = index[key]
        clears, left, turns = map(int, (row["clears"], row["left"], row["turns"]))
        if left + turns != int(record["moves"]):
            raise ValueError(f"Move conservation failed at {identifier}")
        counts[key].update({"episodes": 1, "wins": int(clears >= int(record["goal"])), "early": int(left >= 1),
                            "challenge": int(clears >= int(record["goal"]) + 3), "chain": int(row["chain"]),
                            "productive_extra": int(row["productive_extra"]), "total_turns": turns,
                            "proposal_attempts": int(row["proposals"]), "witness_fallbacks": int(row["fallbacks"])})
    if set(counts) != set(index):
        raise ValueError("Episode ledger groups did not match summary rows")
    for key, totals in counts.items():
        if any(value != int(index[key][field]) for field, value in totals.items()):
            raise ValueError(f"Episode ledger did not reconcile at {key}")


def check_exact_replay(path, archived, identifiers):
    replayed = index_unique(rows(path), ("id", "agent"))
    assert set(replayed) == {(id, agent) for id in identifiers for agent in AGENTS}
    fields = ("exact_early", "exact_challenge", "exact_chain", "exact_mean_productive_extra_tiles")
    error = {key: max(abs(float(row[key]) - float(archived[id][key])) for id, row in replayed.items()) for key in fields}
    if max(error.values()) >= 1e-10:
        raise ValueError("Witness replay diverged from the archived exact probabilities")
    stats = {"archived_exact_max_error": error}
    paired = None
    if all("paired_" + field in next(iter(replayed.values())) for field in fields):
        errors = {id: {field: abs(float(row["paired_" + field]) - float(row[field])) for field in fields}
                  for id, row in replayed.items()}
        assert all(max(error.values()) < 1e-10 for id, error in errors.items() if id[1] == "Random")
        stats["agent_rows_changed_by_canonical_index"] = sum(max(error.values()) >= 1e-10 for error in errors.values())
        stats["max_change_by_canonical_index"] = {field: max(row[field] for row in errors.values()) for field in fields}
        paired = {id: exact_probabilities({"goal": archived[id]["goal"], **{field: row["paired_" + field] for field in fields}})
                  for id, row in replayed.items()}
    return stats, paired


def check_oracle_ledger(path, summary, identifiers):
    physical = index_unique(rows(path), ("id",))
    assert set(physical) == {(id,) for id in identifiers}
    grouped = defaultdict(Counter)
    for row in summary:
        grouped[row["id"]].update({key: int(row[key]) for key in ("proposal_safe", "proposal_attempts", "solver_calls", "cache_hits")})
    mapping = {"proposal_safe": "natural_edges_replayed", "proposal_attempts": "proposals",
               "solver_calls": "solver_calls", "cache_hits": "cache_hits"}
    for (id,), row in physical.items():
        assert all(grouped[id][a] == int(row[b]) for a, b in mapping.items()), id
    totals = {key: sum(int(row[key]) for row in physical.values()) for key in ("proposals", "cache_hits", "solver_calls",
              "certificates_verified", "natural_edges_replayed", "candidate_checks", "refill_nodes", "states")}
    return totals


def net_tier_shift_interval(details, identifiers_by_recipe, controller, model):
    names = tuple(sorted(identifiers_by_recipe))
    by_mode = {(row["id"], row["controller"]): row for row in details}

    def shifted(id, mode):
        row = by_mode[id, mode]
        return int(row[f"{model}_tier_witness"] != row[f"{model}_tier_mc"])

    differences = [[shifted(id, controller) - shifted(id, "Witness") for id in identifiers_by_recipe[recipe]]
                   for recipe in names]
    n = sum(map(len, differences))
    rng = random.Random(202611243)
    sampled = sorted(sum(value for group in differences for value in rng.choices(group, k=len(group))) / n
                     for _ in range(3000))
    return {"difference": sum(map(sum, differences)) / n,
            "paired_board_bootstrap_ci95": [sampled[74], sampled[2924]]}


def compare_scores(identifiers, reference, evaluated, cuts, recipe, mode):
    result = []
    for model in MODELS:
        first = [reference[id][model] for id in identifiers]
        second = [evaluated[id][model] for id in identifiers]
        cutoff = cuts[model]
        tiers_0 = [label(value, cutoff) for value in first]
        tiers_1 = [label(value, cutoff) for value in second]
        changed = sum(a != b for a, b in zip(tiers_0, tiers_1))
        extreme = sum({a, b} == {"hard", "easy"} for a, b in zip(tiers_0, tiers_1))
        tier_counts = Counter(tiers_1)
        result.append({"controller": mode, "recipe": recipe, "model": model, "n": len(first),
                       "rho_vs_exact_witness": rho(first, second), "tier_changes": changed,
                       "two_tier_changes": extreme, "tier_change_rate": changed / len(first),
                       "mean_score_change": statistics.mean(b-a for a, b in zip(first, second)),
                       "mean_abs_score_change": statistics.mean(abs(b-a) for a, b in zip(first, second)),
                       **{tier: tier_counts[tier] for tier in TIERS}})
    return result


def cross_agent(identifiers, scores_by_id, metrics, cuts, mode, recipe):
    result = []
    for model in MODELS:
        input_scores = [scores_by_id[id][model] for id in identifiers]
        tiers = [label(value, cuts[model]) for value in input_scores]
        for agent in AGENTS:
            for endpoint in ENDPOINTS:
                values = [metrics[id, agent][endpoint] for id in identifiers]
                by_tier = {tier: [value for value, key in zip(values, tiers) if key == tier] for tier in TIERS}
                means = {tier: statistics.mean(series) if series else None for tier, series in by_tier.items()}
                result.append({"controller": mode, "recipe": recipe, "model": model, "agent": agent,
                               "endpoint": endpoint, "rho": probability_ranks(input_scores, values),
                               **{f"n_{tier}": len(series) for tier, series in by_tier.items()},
                               **{f"mean_{tier}": value for tier, value in means.items()}})
    return result


def main():
    parser = argparse.ArgumentParser(description="Compare bounded Strict refill controls on a paired certified cohort")
    parser.add_argument("summary", type=Path)
    parser.add_argument("exact", type=Path)
    parser.add_argument("cuts", type=Path, help="JSON mapping V1, V3 and V4 to a pair of calibrated cutoffs")
    parser.add_argument("output", type=Path)
    parser.add_argument("--episodes", type=Path, help="Optional underlying episode ledger to reconcile")
    parser.add_argument("--witness-exact", type=Path, help="Recomputed Witness proof probabilities")
    parser.add_argument("--oracle", type=Path, help="Per-board proof and replay counters")
    parser.add_argument("--plan", type=Path, default=Path(__file__).with_name("analysis-plan.json"))
    args = parser.parse_args()
    plan = read_json(args.plan)
    cuts = read_json(args.cuts)
    assert set(cuts) == set(MODELS) and all(len(value) == 2 and 0 < value[0] < value[1] < 1 for value in cuts.values())
    assert cuts["V1"] == [0.554, 0.651]
    raw = rows(args.summary)
    index = index_unique(raw, ("id", "agent", "controller"))
    if args.episodes:
        check_episode_ledger(args.episodes, index)
    accepted = index_unique(rows(args.exact), ("id", "agent"))
    identifiers = sorted({key[0] for key in index})
    assert set(index) == {(id, agent, mode) for id in identifiers for agent in AGENTS for mode in MODES}
    expected_episodes = plan["sample_plan"]["episodes_per_board_agent_controller"]
    assert {int(row["episodes"]) for row in raw} == {expected_episodes}
    recipes = {id: index[id, "Random", "Witness"]["recipe"] for id in identifiers}
    assert all(recipes[row["id"]] == row["recipe"] for row in raw)
    assert set(recipes.values()) == set(RECIPES)
    group_ids = {recipe: [id for id in identifiers if recipes[id] == recipe] for recipe in RECIPES}
    assert len({len(ids) for ids in group_ids.values()}) == 1
    exact = {(id, agent): exact_probabilities(accepted[id, agent]) for id in identifiers for agent in AGENTS}
    exact_scores = {id: scores(exact[id, "Random"]) for id in identifiers}
    verification = {}
    paired = None
    if args.witness_exact:
        verification["witness_exact"], paired = check_exact_replay(args.witness_exact, accepted, identifiers)
    if args.oracle:
        verification["oracle_counters"] = check_oracle_ledger(args.oracle, raw, identifiers)

    changes, agreement, individual = [], [], []
    for recipe, ids in group_ids.items():
        agreement.extend(cross_agent(ids, exact_scores, exact, cuts, "WitnessExact", recipe))
        if paired is not None:
            agreement.extend(cross_agent(ids, exact_scores, paired, cuts, "WitnessExactPaired", recipe))
    grouped = defaultdict(list)
    for row in raw:
        grouped[row["controller"], row["recipe"]].append(row)
        grouped[row["controller"], "all"].append(row)
    telemetry = {mode: {name: summarize_execution(grouped[mode, name], mode, plan) for name in (*RECIPES, "all")}
                 for mode in MODES}
    for mode in MODES:
        metrics = {(id, agent): estimated_probabilities(index[id, agent, mode]) for id in identifiers for agent in AGENTS}
        candidates = {id: scores(metrics[id, "Random"]) for id in identifiers}
        for recipe, ids in group_ids.items():
            changes.extend(compare_scores(ids, exact_scores, candidates, cuts, recipe, mode))
            agreement.extend(cross_agent(ids, candidates, metrics, cuts, mode, recipe))
        for id in identifiers:
            detail = {"id": id, "recipe": recipes[id], "controller": mode}
            detail.update(metrics[id, "Random"])
            for model in MODELS:
                detail[f"{model}_exact_witness"] = exact_scores[id][model]
                detail[f"{model}_mc"] = candidates[id][model]
                detail[f"{model}_tier_witness"] = label(exact_scores[id][model], cuts[model])
                detail[f"{model}_tier_mc"] = label(candidates[id][model], cuts[model])
            individual.append(detail)

    aggregate_changes = {}
    for mode in MODES:
        aggregate_changes[mode] = {}
        for model in MODELS:
            selection = [row for row in changes if row["controller"] == mode and row["model"] == model]
            valid = [row["rho_vs_exact_witness"] for row in selection if row["rho_vs_exact_witness"] is not None]
            rank = statistics.mean(valid)
            shift = sum(row["tier_changes"] for row in selection)
            aggregate_changes[mode][model] = {
                "macro_within_recipe_rho": rank,
                "tier_changes": shift,
                "tier_change_rate": shift / len(identifiers),
                "two_tier_changes": sum(row["two_tier_changes"] for row in selection),
                "mean_abs_score_change": statistics.mean(row["mean_abs_score_change"] for row in selection),
                "mean_score_change": statistics.mean(row["mean_score_change"] for row in selection),
                "rank_gate": rank >= plan["diagnostic_thresholds"]["minimum_macro_within_recipe_rank_correlation"],
                "tier_gate": shift / len(identifiers) <= plan["diagnostic_thresholds"]["maximum_tier_change_rate"],
            }
            if mode != "Witness":
                aggregate_changes[mode][model]["excess_tier_shift_vs_witness_mc"] = net_tier_shift_interval(
                    individual, group_ids, mode, model)

    # Refill modes can make previously constant outcomes vary. Fix the set of
    # report cells to their intersection when comparing macro correlations.
    cells_per_mode = {}
    reference_modes = (*MODES, "WitnessExact", "WitnessExactPaired") if paired is not None else (*MODES, "WitnessExact")
    for mode in reference_modes:
        cells_per_mode[mode] = {(row["recipe"], row["agent"], row["endpoint"]) for row in agreement
                                if row["controller"] == mode and row["model"] == "V1" and row["agent"] != "Random"
                                and row["rho"] is not None}
    shared = set.intersection(*cells_per_mode.values())
    cross = {
        mode: {model: statistics.mean(row["rho"] for row in agreement
                                      if row["controller"] == mode and row["model"] == model
                                      and (row["recipe"], row["agent"], row["endpoint"]) in shared)
               for model in MODELS}
        for mode in reference_modes
    }

    summary = {"cohort_size": len(identifiers), "episodes_per_board_agent_controller": expected_episodes,
               "cuts": cuts, "plan_sha256": hashlib.sha256(args.plan.read_bytes()).hexdigest(),
               "changes_vs_exact_witness": aggregate_changes, "execution": telemetry,
               "cross_agent_cells_shared": len(shared), "cross_agent_macro_shared_cells": cross, "verification": verification}
    args.output.mkdir(exist_ok=True, parents=True)
    write_csv(args.output / "controller-board-scores.csv", individual)
    write_csv(args.output / "controller-rank-by-recipe.csv", changes)
    write_csv(args.output / "controller-agent-by-recipe.csv", agreement)
    (args.output / "controller-summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"boards": len(identifiers), "changes": aggregate_changes, "cross_agent_macro": cross}, indent=2))


if __name__ == "__main__":
    main()
