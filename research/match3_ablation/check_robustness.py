import argparse
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path

import ablation


EVENTS = ("exact_early", "exact_challenge", "exact_chain")
MODELS = ("V1", "V2", "V3")
TIERS = ("hard", "normal", "easy")


def midpoint_ranks(values):
    order = sorted(range(len(values)), key=values.__getitem__)
    output = [0.0] * len(order)
    start = 0
    while start < len(order):
        end = start + 1
        while end < len(order) and values[order[end]] == values[order[start]]:
            end += 1
        for index in order[start:end]:
            output[index] = (1 + start + end) / 2
        start = end
    return output


def rho(first, second):
    if len(set(first)) <= 1 or len(set(second)) <= 1:
        return 0.0
    return statistics.correlation(midpoint_ranks(first), midpoint_ranks(second))


def format_groups(values):
    return {key: statistics.fmean(group) for key, group in values.items()}


def check_old_features(base):
    largest = {}
    for name, original_path in (
        ("calibration", base.parent / "match3_difficulty" / "results" / "policies.csv"),
        ("holdout", base.parent / "match3_pool" / "holdout_sim" / "policies.csv"),
    ):
        scores = ablation.unique(ablation.read_csv(base / f"{name}-cascade.csv"), lambda row: (row["id"], row["agent"]))
        if not original_path.is_file():
            continue
        originals = ablation.unique(ablation.read_csv(original_path), lambda row: (row["id"], row["agent"]))
        ablation.require(set(scores) == set(originals), f"Score and original keys differ for {name}")
        largest[name] = {field: max(abs(float(scores[key][field]) - float(originals[key][field])) for key in scores)
                         for field in EVENTS}
        ablation.require(max(largest[name].values()) < 1e-10, f"Exact event parity changed for {name}")
    return largest


def check_fresh_scores(candidates, features, published, cuts):
    candidates = ablation.unique((row for row in candidates if row["cert_verified"] == "1"), lambda row: row["id"])
    features = ablation.unique(features, lambda row: (row["id"], row["agent"]))
    published = ablation.unique(published, lambda row: row["id"])
    ablation.require(set(candidates) == set(published), "Published IDs differ from confirmed WIN IDs")
    for key, row in features.items():
        early, challenge, chain = (float(row[field]) for field in EVENTS)
        joint = float(row["exact_early_and_challenge"])
        ablation.require(max(0.0, early + challenge - 1) - 1e-12 <= joint <= min(early, challenge) + 1e-12,
                         f"Joint event is inconsistent at {key}")
        ablation.require(abs(chain - float(row["exact_extra_ge3"])) < 1e-12,
                         f"Chain event differs from three or more extra cleared tiles at {key}")
    score_error = 0.0
    by_recipe = defaultdict(lambda: {model: Counter(dict.fromkeys(TIERS, 0)) for model in MODELS})
    for key, candidate in candidates.items():
        row = features[key, "Random"]
        early, challenge, chain = (float(row[field]) for field in EVENTS)
        value = float(row["exact_extra_tiles_capped6"])
        scores = {"V1": (early + challenge + chain) / 3, "V2": (early + challenge + value) / 3,
                  "V3": (early + challenge) / 2}
        for model, score in scores.items():
            first_cut, second_cut = cuts[model]["cuts"]
            tier = "easy" if score >= second_cut else "normal" if score >= first_cut else "hard"
            expected = published[key]
            score_error = max(score_error, abs(float(expected[f"{model}_ease"]) - score))
            ablation.require(expected[f"{model}_tier"] == tier, f"Published tier differs for {key} {model}")
            by_recipe[candidate["stratum"]][model][tier] += 1
    ablation.require(score_error < 1e-12, f"Published score differs by {score_error}")
    return dict(by_recipe), score_error


def main():
    parser = argparse.ArgumentParser(description="Independent confirmation metric and bootstrap sensitivity")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--replicates", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=202611242)
    args = parser.parse_args()
    base = args.root
    results = base / "results"
    protocol = ablation.load_json(base / "protocol.json")
    lock = ablation.load_json(results / "selection-lock.json")
    original = ablation.load_json(results / "confirmation-summary.json")
    fitted = lock["fitted"]
    candidates_path = base / "fresh" / "dataset" / "candidates.csv"
    features_path = base / "fresh" / "confirmation-cascade.csv"
    candidates, cohort = ablation.load_cohort(candidates_path, features_path, protocol, protocol["confirmation_n"])
    coverage, score_error = check_fresh_scores(candidates, ablation.read_csv(features_path),
                                              ablation.read_csv(results / "confirmation-labels.csv"), fitted)

    labels = ablation.scored_rows(cohort, fitted)
    views = ablation.paired_views(cohort, labels, fitted, protocol)
    reference = protocol["scoring"]["reference_agent"]
    cells = ablation.select_cells(views, reference)
    point = {model: [] for model in MODELS}
    point_rows = []
    summaries = {model: {group: defaultdict(list) for group in ("agent", "endpoint", "recipe")} for model in MODELS}
    expected_cells = {(cell["model"], cell["stratum"], cell["agent"], cell["endpoint"]): cell
                      for cell in ablation.read_csv(results / "confirmation-recipe-agent.csv")}
    maximum_cell_error = 0.0
    for recipe, (scores, _, probes) in views.items():
        for key, outcomes in probes.items():
            for model in MODELS:
                agent, endpoint = key
                cell = expected_cells[model, recipe, agent, endpoint]
                primary = key in cells[recipe]
                ablation.require(bool(int(cell["primary"])) == primary, f"Primary cell status differs for {recipe} {key}")
                if len(set(outcomes)) == 1:
                    ablation.require(not cell["rho"], f"Constant endpoint was assigned a correlation for {recipe} {key}")
                    continue
                computed = rho(scores[model], outcomes)
                error = abs(computed - float(cell["rho"]))
                maximum_cell_error = max(maximum_cell_error, error)
                if primary:
                    point[model].append(computed)
                    summaries[model]["agent"][agent].append(computed)
                    summaries[model]["endpoint"][endpoint].append(computed)
                    summaries[model]["recipe"][recipe].append(computed)
                    point_rows.append({"recipe": recipe, "agent": agent, "endpoint": endpoint, "model": model,
                                       "rho": computed})

    macro = {model: statistics.fmean(values) for model, values in point.items()}
    ablation.require(maximum_cell_error < 1e-12, f"Cell correlations differ by {maximum_cell_error}")
    ablation.require(all(math.isclose(macro[model], original["models"][model]["macro_rho"], abs_tol=1e-12)
                         for model in MODELS), "Macro correlations differ")
    protocol["selection"]["bootstrap"].update(replicates=args.replicates, seed=args.seed)
    interval = ablation.paired_bootstrap(views, cells, protocol)
    margin = protocol["selection"]["practical_margin_rho"]
    output = {
        "bootstrap_replicates": args.replicates,
        "bootstrap_seed": args.seed,
        "n_certified_levels": len(cohort),
        "primary_nonconstant_cells": len(point["V1"]),
        "maximum_absolute_score_error": score_error,
        "maximum_absolute_cell_rho_error": maximum_cell_error,
        "previous_exact_event_parity_max_abs_error": check_old_features(base),
        "macro_rho": macro,
        "model_details": {model: {f"by_{group}": format_groups(values) for group, values in groups.items()}
                          for model, groups in summaries.items()},
        "differences_vs_v1": {model: {"delta": macro[model] - macro["V1"], "ci95": interval[model],
                                       "original_ci95": original["comparisons_vs_v1"][model]["ci95"]}
                               for model in MODELS[1:]},
        "v3_noninferiority_margin": -margin,
        "v3_ci_lower_distance_above_margin": interval["V3"][0] + margin,
        "v3_ci_excludes_a_loss_larger_than_margin": interval["V3"][0] > -margin,
        "tier_coverage": coverage,
        "primary_cell_rho": point_rows,
    }
    ablation.write_json(results / "confirmation-sensitivity.json", output)
    print(f"{len(cohort)} levels, {len(point['V1'])} cells, {args.replicates} resamples")
    print(f"V3 delta={macro['V3'] - macro['V1']:.7f}, CI={interval['V3']}; margin={-margin:.4f}")
    print(f"g8-m2 V3 tier coverage: {dict(coverage['g8-m2']['V3'])}")


if __name__ == "__main__":
    main()
