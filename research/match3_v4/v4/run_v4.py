import argparse
import json
import math
import random
import statistics
import subprocess
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


MODELS = ("V1", "V3", "V4")
TIERS = ("hard", "normal", "easy")


def add_productive_metric(boards):
    for board in boards:
        goal = int(board["goal"])
        if goal <= 0:
            raise ValueError(f"Nonpositive goal for {board['id']}")
        for row in board["agents"].values():
            rate = row["exact_mean_productive_extra_tiles"] / goal
            if not 0 <= rate <= 1 + 1e-12:
                raise ValueError(f"Goal-contributing extra clears out of range at {board['id']}")
            row["productive_rate"] = rate
            row["expected_overshoot"] = row["exact_mean_clears"] - goal


def board_scores(ab, cohort, fitted):
    rows = ab.scored_rows(cohort, fitted)
    for board, row in zip(cohort, rows):
        random_agent = board["agents"]["Random"]
        for field in ("exact_mean_productive_extra_tiles", "productive_rate", "expected_overshoot"):
            row[field] = random_agent[field]
    return rows


def check_control_scores(ab, labels, previous, models):
    reference = ab.unique(ab.read_csv(previous), lambda row: row["id"])
    ab.require(set(reference) == {row["id"] for row in labels}, f"Changed IDs: {previous}")
    for row in labels:
        old = reference[row["id"]]
        for model in models:
            ab.require(row[f"{model}_tier"] == old[f"{model}_tier"] and
                       abs(row[f"{model}_ease"] - float(old[f"{model}_ease"])) < 1e-12,
                       f"Frozen {model} score changed at {row['id']}")


def independent_replay(verifier, data, boards):
    def check(board):
        name = board["id"]
        process = subprocess.run([str(verifier), "verify", str(data / "boards" / f"{name}.txt"),
                                  str(board["goal"]), str(board["moves"]),
                                  str(data / "certificates" / f"{name}.m3cert")],
                                 capture_output=True, text=True, check=True)
        if process.stdout.strip() != "PASS":
            raise ValueError(f"Independent replay failed: {name}: {process.stdout}")

    with ThreadPoolExecutor(max_workers=4) as executor:
        return sum(1 for _ in executor.map(check, boards))


def bootstrap(ab, views, selected, config):
    generator = random.Random(config["seed"])
    differences = {"V3_minus_V1": [], "V4_minus_V1": [], "V4_minus_V3": []}
    n_cells = sum(map(len, selected.values()))
    for _ in range(config["replicates"]):
        totals = dict.fromkeys(MODELS, 0.0)
        for stratum, (scores, _, _) in views.items():
            n = len(scores["V1"])
            indices = generator.choices(range(n), k=n)
            ranks = {model: ab.rank_vector([scores[model][i] for i in indices]) for model in MODELS}
            for field in selected[stratum].values():
                outcome = ab.rank_vector([field[i] for i in indices])
                for model in MODELS:
                    totals[model] += ab.association(ranks[model], outcome)
        for key, deltas in differences.items():
            a, b = key.split("_minus_")
            deltas.append((totals[a] - totals[b]) / n_cells)
    tail = (1 - config["ci_level"]) / 2
    return {key: [ab.pct(vector, tail), ab.pct(vector, 1 - tail)] for key, vector in differences.items()}


def normalize(vector):
    norm = math.sqrt(sum(value * value for value in vector))
    return [value / norm for value in vector] if norm > 1e-10 else None


def residualize(ab, vector, basis):
    result = list(vector)
    for orthogonal in basis:
        coefficient = ab.association(result, orthogonal)
        result = [value - coefficient * unit for value, unit in zip(result, orthogonal)]
    return result


def rank_overlap(ab, cohort, protocol):
    overlap, partial = [], []
    feature_fields = (*ab.EVENTS, "expected_overshoot")
    digits = protocol["scoring"]["probability_round_decimals_for_ranks"]
    for recipe in protocol["strata"]:
        boards = [board for board in cohort if board["stratum"] == recipe]

        def rank(agent, field):
            return ab.rank_vector([round(board["agents"][agent][field], digits) for board in boards])

        for agent in protocol["agents"]:
            productive = rank(agent, "productive_rate")
            for feature in feature_fields:
                target = rank(agent, feature)
                overlap.append({"stratum": recipe, "agent": agent, "feature": feature, "n": len(boards),
                                "rho": ab.association(productive, target) if productive and target else None})

        basis = []
        for feature in ab.ENDPOINTS:
            predictor = rank("Random", feature)
            if predictor is not None:
                orthogonal = normalize(residualize(ab, predictor, basis))
                if orthogonal is not None:
                    basis.append(orthogonal)
        raw_productive = rank("Random", "productive_rate")
        productive = normalize(residualize(ab, raw_productive, basis)) if raw_productive is not None else None
        for agent in protocol["agents"][1:]:
            for outcome in ab.ENDPOINTS:
                raw_target = rank(agent, outcome)
                target = normalize(residualize(ab, raw_target, basis)) if raw_target is not None else None
                partial.append({"stratum": recipe, "agent": agent, "endpoint": outcome, "n": len(boards),
                                "controls_with_variation": len(basis), "partial_rho":
                                    ab.association(productive, target) if productive and target else None})
    return overlap, partial


def summarize(cells, labels, recipe_order):
    output = {}
    for model in MODELS:
        scored = [cell for cell in cells if cell["model"] == model and cell["primary"]]
        tier_coverage = {}
        for recipe in recipe_order:
            counts = Counter(row[f"{model}_tier"] for row in labels if row["stratum"] == recipe)
            tier_coverage[recipe] = {tier: counts[tier] for tier in TIERS}
        monotone = [cell["monotonic"] for cell in scored if cell["monotonic"] is not None]
        row = {
            "macro_rho": statistics.mean(cell["rho"] for cell in scored),
            "primary_cells": len(scored),
            "by_recipe": {recipe: statistics.mean(cell["rho"] for cell in scored if cell["stratum"] == recipe)
                          for recipe in recipe_order},
            "by_agent": {agent: statistics.mean(cell["rho"] for cell in scored if cell["agent"] == agent)
                         for agent in sorted({cell["agent"] for cell in scored})},
            "by_endpoint": {endpoint: statistics.mean(cell["rho"] for cell in scored if cell["endpoint"] == endpoint)
                            for endpoint in sorted({cell["endpoint"] for cell in scored})},
            "coverage": tier_coverage,
            "primary_cells_with_nondecreasing_tier_means": sum(monotone),
            "eligible_primary_cells_for_tier_order": len(monotone),
            "same_tier_as_v1": sum(row[f"{model}_tier"] == row["V1_tier"] for row in labels),
            "two_tiers_from_v1": sum({row[f"{model}_tier"], row["V1_tier"]} == {"hard", "easy"} for row in labels),
        }
        output[model] = row
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    here = Path(__file__).resolve().parent
    parser.add_argument("--work", type=Path, default=here.parents[1])
    parser.add_argument("--output", type=Path, default=here / "results")
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--verifier", type=Path, required=True)
    params = parser.parse_args()
    work = params.work.resolve()
    sys.path.insert(0, str(work / "match3_ablation"))
    import ablation as ab

    plan = ab.load_json(here / "protocol.json")
    ablation_dir = work / "match3_ablation"
    scorer = ab.load_json(ablation_dir / "protocol.json")
    baseline = work / "match3_pool/frozen-standard.json"
    lock = ablation_dir / "results/selection-lock.json"
    baseline_sha = ab.digest(baseline)
    lock_sha = ab.digest(lock)
    ab.require(baseline_sha == plan["v1_frozen_sha256"] and lock_sha == plan["v3_lock_sha256"], "Frozen baseline changed")
    source_head = subprocess.check_output(["git", "-C", str(params.solver), "rev-parse", "HEAD"], text=True).strip()
    ab.require(source_head == plan["solver_commit"], "Solver checkout differs from frozen commit")
    source_diff = subprocess.run(["git", "-C", str(params.solver), "diff", "--quiet", "HEAD", "--"], check=False)
    ab.require(source_diff.returncode == 0, "Solver tracked files differ from frozen commit")
    sources = {
        "calibration_candidates": work / "match3_difficulty/dataset/candidates.csv",
        "calibration_policies": work / "match3_difficulty/results/policies.csv",
        "calibration_features": ablation_dir / "calibration-cascade.csv",
        "comparison_candidates": ablation_dir / "fresh/dataset/candidates.csv",
        "comparison_features": ablation_dir / "fresh/confirmation-cascade.csv",
    }
    lock_data = ab.load_json(lock)
    source_hashes = {name: ab.digest(path) for name, path in sources.items()}
    for name in ("calibration_candidates", "calibration_policies", "calibration_features"):
        ab.require(source_hashes[name] == lock_data["input_hashes"][name], f"Changed calibration source: {name}")
    fresh_meta = ab.load_json(ablation_dir / "fresh/generation-report.json")
    ab.require(source_hashes["comparison_candidates"] == fresh_meta["candidate_csv_sha256"] and
               fresh_meta["independent_cli_replays_passed"] == plan["comparison_count"], "Unexpected comparison seed data")
    calibration_raw, cal = ab.load_cohort(sources["calibration_candidates"], sources["calibration_features"], scorer,
                                          plan["calibration_count"], sources["calibration_policies"])
    comparison_raw, boards = ab.load_cohort(sources["comparison_candidates"], sources["comparison_features"], scorer,
                                             plan["comparison_count"])
    ab.check_cohort_seed_disjoint(calibration_raw, comparison_raw)
    add_productive_metric(cal)
    add_productive_metric(boards)
    fitted = {model: lock_data["fitted"][model] for model in ("V1", "V3")}
    ab.require(fitted["V1"]["cuts"] == ab.load_json(baseline)["ease_cutoffs"], "Changed V1 cutoffs")
    v4_fields = plan["v4_components"]
    fitted["V4"] = {"components": v4_fields, "cuts": ab.midpoints(sorted(ab.values(row["agents"]["Random"], v4_fields)
                                                                       for row in cal))}
    cal_labels = board_scores(ab, cal, fitted)
    labels = board_scores(ab, boards, fitted)
    check_control_scores(ab, cal_labels, ablation_dir / "results/calibration-labels.csv", ("V1", "V3"))
    check_control_scores(ab, labels, ablation_dir / "results/confirmation-labels.csv", ("V1", "V3"))

    params.output.mkdir(parents=True, exist_ok=True)
    views = ab.paired_views(boards, labels, fitted, scorer)
    selected = ab.select_cells(views, "Random")
    cells = ab.cell_statistics(views, fitted, "Random")
    models = summarize(cells, labels, scorer["strata"])
    prior_v1 = ab.load_json(ablation_dir / "results/confirmation-summary.json")["models"]["V1"]["macro_rho"]
    ab.require(abs(models["V1"]["macro_rho"] - prior_v1) < 1e-12, "Primary metric differs from the prior comparison")
    intervals = bootstrap(ab, views, selected, plan["bootstrap"])
    comparisons = {key: {"delta": models[key.split("_minus_")[0]]["macro_rho"] -
                                  models[key.split("_minus_")[1]]["macro_rho"], "ci95": ci}
                   for key, ci in intervals.items()}
    overlap, partial = rank_overlap(ab, boards, scorer)
    cli_pass = independent_replay(params.verifier, sources["comparison_candidates"].parent, boards)
    v4_difference = comparisons["V4_minus_V1"]
    recipe_deltas = {recipe: models["V4"]["by_recipe"][recipe] - models["V1"]["by_recipe"][recipe]
                     for recipe in scorer["strata"]}
    gates = {
        "improvement": v4_difference["delta"] >= plan["gates"]["improvement_vs_v1"],
        "paired_interval_lower_above_zero": v4_difference["ci95"][0] > 0.0,
        "recipe_degradation": min(recipe_deltas.values()) >= -plan["gates"]["max_descriptive_recipe_degradation"],
        "all_three_tiers_per_recipe": all(min(counts.values()) > 0 for counts in models["V4"]["coverage"].values()),
        "all_certificates_independently_replayed": cli_pass == plan["comparison_count"],
    }
    summary = {
        "plan_sha256": ab.digest(here / "protocol.json"), "inputs_sha256": source_hashes,
        "v1_sha256": baseline_sha, "v3_lock_sha256": lock_sha,
        "fitted": fitted, "calibration_tiers": {model: dict(Counter(row[f"{model}_tier"] for row in cal_labels)) for model in MODELS},
        "comparison_certificates_replayed": cli_pass, "diagnostics": {
            "productive_factor_mean_random": statistics.mean(row["productive_rate"] for row in labels),
            "partial_rank_macro": statistics.mean(row["partial_rho"] for row in partial if row["partial_rho"] is not None),
            "partial_rank_nonconstant_cells": sum(row["partial_rho"] is not None for row in partial),
        },
        "models": models, "comparisons": comparisons, "v4_recipe_deltas_vs_v1": recipe_deltas, "v4_gates": gates,
        "fixed_witness_gate_passed": all(gates.values()), "production_standard": "V1",
    }
    ab.write_csv(params.output / "calibration-labels.csv", cal_labels)
    ab.write_csv(params.output / "comparison-labels.csv", labels)
    ab.write_csv(params.output / "recipe-agent.csv", cells)
    ab.write_csv(params.output / "feature-overlap.csv", overlap)
    ab.write_csv(params.output / "partial-rank.csv", partial)
    ab.write_json(params.output / "summary.json", summary)
    print(json.dumps({"scores": {key: value["macro_rho"] for key, value in models.items()}, "v4_vs_v1": v4_difference,
                      "recipe_deltas": recipe_deltas, "gates": gates}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
