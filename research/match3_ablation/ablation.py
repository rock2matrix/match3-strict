import argparse
import csv
import hashlib
import json
import math
import random
import statistics
from collections import Counter
from pathlib import Path


MODELS = ("V1", "V2", "V3")
TIERS = ("hard", "normal", "easy")
ENDPOINTS = ("exact_early", "exact_challenge")
EVENTS = (*ENDPOINTS, "exact_chain")
VALUE_FEATURES = ("exact_extra_tiles_capped6", "exact_extra_tiles_capped9", "exact_mean_extra_tiles")


def read_csv(file):
    with Path(file).open(encoding="utf-8-sig", newline="") as source:
        return list(csv.DictReader(source))


def write_csv(file, rows):
    with Path(file).open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def digest(file):
    return hashlib.sha256(Path(file).read_bytes()).hexdigest()


def load_json(file):
    return json.loads(Path(file).read_text(encoding="utf-8"))


def write_json(file, data, mode="w"):
    with Path(file).open(mode, encoding="utf-8") as target:
        json.dump(data, target, ensure_ascii=False, indent=2, allow_nan=False)
        target.write("\n")


def unique(rows, key):
    keyed = {}
    for row in rows:
        name = key(row)
        if name in keyed:
            raise ValueError(f"Duplicate {name}")
        keyed[name] = row
    return keyed


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_cohort(candidates_path, features_path, protocol, count, original_policy_path=None):
    candidates = read_csv(candidates_path)
    unique(candidates, lambda row: row["id"])
    unique(candidates, lambda row: row["seed"])
    certified = [row for row in candidates if row["cert_verified"] == "1"]
    require(len(certified) == count, f"Expected {count} verified boards in {candidates_path}")
    require({row["id"] for row in certified} == {row["id"] for row in candidates if row["verdict"] == "WIN_CERTIFIED"},
            "WIN verdict and verified flag differ")
    strata_counts = Counter(row["stratum"] for row in certified)
    require(strata_counts == {stratum: count // len(protocol["strata"]) for stratum in protocol["strata"]},
            "Verified count deviates from balanced cohort design")

    metrics = unique(read_csv(features_path), lambda row: (row["id"], row["agent"]))
    keys = {(row["id"], agent) for row in certified for agent in protocol["agents"]}
    require(set(metrics) == keys, "Agent rows do not match certified boards")
    originals = unique(read_csv(original_policy_path), lambda row: (row["id"], row["agent"])) if original_policy_path else None
    if originals is not None:
        require(set(originals) == keys, "Original policy rows do not match certified boards")

    result = []
    for candidate in certified:
        agents = {}
        for agent in protocol["agents"]:
            key = (candidate["id"], agent)
            row = metrics[key]
            require(all(int(row[field]) == int(candidate[field]) for field in ("goal", "moves")), f"Unexpected rules for {key}")
            data = {field: float(row[field]) for field in (*EVENTS, *VALUE_FEATURES)}
            require(all(-1e-10 <= data[field] <= 1 + 1e-10 for field in (*EVENTS, *VALUE_FEATURES[:2])),
                    f"Probability outside [0,1] for {key}")
            require(0 <= data["exact_mean_extra_tiles"] and
                    data["exact_extra_tiles_capped9"] <= data["exact_extra_tiles_capped6"] + 1e-10 and
                    data["exact_extra_tiles_capped6"] <= data["exact_chain"] + 1e-10,
                    f"Extra-wave masks are inconsistent for {key}")
            if originals is not None:
                for field in EVENTS:
                    exact = float(originals[key][field])
                    require(abs(data[field] - exact) < 1e-10, f"Existing exact probability differs for {field} at {key}")
                    data[field] = exact
            mean = data["exact_mean_extra_tiles"]
            data["mean_extra_tiles_over_mean_plus_3"] = mean / (mean + 3)
            for field in ("exact_early_and_challenge", "exact_mean_productive_extra_tiles", "exact_extra_ge3",
                          "exact_extra_ge6", "exact_extra_ge9", "exact_mean_clears"):
                if field in row:
                    data[field] = float(row[field])
            agents[agent] = data
        result.append({**{k: candidate[k] for k in ("id", "seed", "stratum", "goal", "moves")}, "agents": agents})
    return candidates, result


def values(scores, fields):
    return statistics.mean(scores[field] for field in fields)


def midpoints(ordered, positions=(33, 66)):
    require(len(ordered) >= max(positions) + 1, "Too few observations to fit cutoffs")
    return [statistics.mean((ordered[index - 1], ordered[index])) for index in positions]


def fit(calibration, protocol):
    variants = protocol["variants"]
    fields = {**{model: spec["components"] for model, spec in variants.items()}, **protocol["sensitivity_variants"]}
    fits = {}
    for model, features in fields.items():
        if model == "V1":
            cuts = variants[model]["cuts"]
        else:
            ordered = sorted(values(row["agents"]["Random"], features) for row in calibration)
            cuts = midpoints(ordered, variants.get(model, {}).get("calibration_cuts_after_sorted_positions", (33, 66)))
        fits[model] = {"components": features, "cuts": cuts}
    return fits


def label(ease, cuts):
    return "hard" if ease < cuts[0] else "normal" if ease < cuts[1] else "easy"


def scored_rows(cohort, fitted):
    result = []
    for board in cohort:
        metrics = board["agents"]["Random"]
        row = {k: board[k] for k in ("id", "seed", "stratum", "goal", "moves")}
        row.update({field: metrics[field] for field in (*EVENTS, *VALUE_FEATURES)})
        for model, spec in fitted.items():
            ease = values(metrics, spec["components"])
            row[f"{model}_ease"] = ease
            row[f"{model}_tier"] = label(ease, spec["cuts"])
        result.append(row)
    return result


def check_v1(labels, path):
    original = unique(read_csv(path), lambda row: row["id"])
    require({row["id"] for row in labels} == set(original), "V1 reference IDs differ")
    for row in labels:
        before = original[row["id"]]
        require(row["V1_tier"] == before["tier"] and abs(row["V1_ease"] - float(before["ease"])) < 1e-10,
                f"Frozen V1 deviated at {row['id']}")


def rank_vector(vector):
    order = sorted(range(len(vector)), key=vector.__getitem__)
    centered = [0.0] * len(vector)
    start = 0
    norm = 0.0
    while start < len(order):
        end = start + 1
        while end < len(order) and vector[order[end]] == vector[order[start]]:
            end += 1
        centered_rank = (start + end - len(order)) / 2
        norm += (end - start) * centered_rank * centered_rank
        for index in order[start:end]:
            centered[index] = centered_rank
        start = end
    scale = math.sqrt(norm)
    return [value / scale for value in centered] if scale else None


def association(first, second):
    if first is None or second is None:
        return 0.0
    return sum(a * b for a, b in zip(first, second))


def paired_views(cohort, labels, fitted, protocol):
    result = {}
    digits = protocol["scoring"]["probability_round_decimals_for_ranks"]
    for stratum in protocol["strata"]:
        points = [(board, row) for board, row in zip(cohort, labels) if board["stratum"] == stratum]
        results = {model: [round(row[f"{model}_ease"], digits) for _, row in points] for model in fitted}
        tier_map = {model: [row[f"{model}_tier"] for _, row in points] for model in fitted}
        probes = {(agent, endpoint): [round(board["agents"][agent][endpoint], digits) for board, _ in points]
                  for agent in protocol["agents"] for endpoint in ENDPOINTS}
        result[stratum] = (results, tier_map, probes)
    return result


def select_cells(views, reference):
    return {stratum: {key: vector for key, vector in probes.items()
                      if key[0] != reference and len(set(vector)) > 1}
            for stratum, (_, _, probes) in views.items()}


def cell_statistics(views, fitted, reference):
    cells = []
    for stratum, (results, tier_map, probes) in views.items():
        probe_ranks = {key: rank_vector(vector) for key, vector in probes.items()}
        for model in fitted:
            src = rank_vector(results[model])
            for (agent, endpoint), vector in probes.items():
                target = probe_ranks[agent, endpoint]
                by_tier = {tier: [value for value, group in zip(vector, tier_map[model]) if group == tier] for tier in TIERS}
                means = {tier: statistics.mean(series) if series else None for tier, series in by_tier.items()}
                complete = all(value is not None for value in means.values())
                monotonic = (means["normal"] >= means["hard"] - 1e-12 and
                             means["easy"] >= means["normal"] - 1e-12) if complete and target is not None else None
                cells.append({
                    "model": model, "stratum": stratum, "agent": agent, "endpoint": endpoint, "n": len(vector),
                    "distinct_outcomes": len(set(vector)), "primary": int(agent != reference and target is not None),
                    "rho": association(src, target) if target is not None else None,
                    **{f"n_{tier}": len(series) for tier, series in by_tier.items()},
                    **{f"mean_{tier}": value for tier, value in means.items()},
                    "easy_minus_hard": means["easy"] - means["hard"] if means["easy"] is not None and means["hard"] is not None else None,
                    "monotonic": int(monotonic) if monotonic is not None else None,
                })
    return cells


def pct(values, quantile):
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    left = math.floor(position)
    right = math.ceil(position)
    return ordered[left] + (ordered[right] - ordered[left]) * (position - left)


def paired_bootstrap(views, cells, protocol):
    config = protocol["selection"]["bootstrap"]
    randomizer = random.Random(config["seed"])
    deltas = {model: [] for model in MODELS[1:]}
    n_cells = sum(map(len, cells.values()))
    for _ in range(config["replicates"]):
        totals = dict.fromkeys(MODELS, 0.0)
        for stratum, (models, _, _) in views.items():
            n = len(models["V1"])
            indices = [randomizer.randrange(n) for _ in range(n)]
            model_ranks = {model: rank_vector([models[model][i] for i in indices]) for model in MODELS}
            for vector in cells[stratum].values():
                targets = rank_vector([vector[i] for i in indices])
                for model in MODELS:
                    totals[model] += association(model_ranks[model], targets)
        for model in deltas:
            deltas[model].append((totals[model] - totals["V1"]) / n_cells)

    tail = (1 - config["ci_level"]) / 2
    return {model: [pct(values, tail), pct(values, 1 - tail)] for model, values in deltas.items()}


def summarize(candidates, cohort, labels, fitted, protocol):
    views = paired_views(cohort, labels, fitted, protocol)
    reference = protocol["scoring"]["reference_agent"]
    primary = select_cells(views, reference)
    cells = cell_statistics(views, fitted, reference)
    models = {}
    for model in fitted:
        eligible = [cell for cell in cells if cell["model"] == model and cell["primary"]]
        by_tier = Counter(row[f"{model}_tier"] for row in labels)
        has_order = [cell["monotonic"] for cell in eligible if cell["monotonic"] is not None]
        models[model] = {
            "macro_rho": statistics.mean(cell["rho"] for cell in eligible),
            "primary_cells": len(eligible),
            "tier_counts": dict(by_tier),
            "monotonic_cells": sum(has_order), "nonconstant_complete_tier_cells": len(has_order),
            "same_tier_as_v1": sum(row[f"{model}_tier"] == row["V1_tier"] for row in labels),
            "two_tiers_from_v1": sum({row[f"{model}_tier"], row["V1_tier"]} == {"hard", "easy"} for row in labels),
            "by_agent": {agent: statistics.mean(cell["rho"] for cell in eligible if cell["agent"] == agent)
                         for agent in protocol["agents"] if any(cell["agent"] == agent for cell in eligible)},
            "by_recipe": {stratum: statistics.mean(cell["rho"] for cell in eligible if cell["stratum"] == stratum)
                          for stratum in protocol["strata"] if any(cell["stratum"] == stratum for cell in eligible)},
            "by_endpoint": {endpoint: statistics.mean(cell["rho"] for cell in eligible if cell["endpoint"] == endpoint)
                            for endpoint in ENDPOINTS if any(cell["endpoint"] == endpoint for cell in eligible)},
        }
    intervals = paired_bootstrap(views, primary, protocol)
    comparisons = {model: {"delta": models[model]["macro_rho"] - models["V1"]["macro_rho"], "ci95": intervals[model]}
                   for model in MODELS[1:]}
    random_extra = [row["agents"]["Random"]["exact_mean_extra_tiles"] for row in cohort]
    return ({"raw_candidates": len(candidates), "verdicts": dict(Counter(row["verdict"] for row in candidates)),
             "certified": len(cohort), "models": models, "comparisons_vs_v1": comparisons,
             "mean_random_extra_clears": statistics.mean(random_extra),
             "median_random_extra_clears": statistics.median(random_extra)}, cells)


def check_cohort_seed_disjoint(*cohorts):
    used = set()
    for rows in cohorts:
        current = {int(row["seed"]) for row in rows}
        require(not used.intersection(current), "Evaluation seed overlap")
        used.update(current)


def explore(args, protocol):
    require(digest(args.frozen_v1) == protocol["frozen_v1_sha256"], "Unexpected V1 frozen standard")
    v1 = load_json(args.frozen_v1)
    require(v1["ease_cutoffs"] == protocol["variants"]["V1"]["cuts"] and v1["reference_agent"] == "Random" and
            v1["weights"] == [1/3] * 3, "V1 scoring changed")
    require(digest(args.calibration_candidates) == v1["calibration_files_sha256"]["candidates.csv"] and
            digest(args.calibration_policies) == v1["calibration_files_sha256"]["policies.csv"], "Calibration inputs changed")
    cal_raw, cal = load_cohort(args.calibration_candidates, args.calibration_features, protocol,
                               protocol["calibration_n"], args.calibration_policies)
    hold_raw, hold = load_cohort(args.holdout_candidates, args.holdout_features, protocol,
                                 protocol["exploratory_n"], args.holdout_policies)
    check_cohort_seed_disjoint(cal_raw, hold_raw)
    fitted = fit(cal, protocol)
    cal_labels, hold_labels = scored_rows(cal, fitted), scored_rows(hold, fitted)
    check_v1(cal_labels, args.calibration_labels)
    check_v1(hold_labels, args.holdout_labels)
    summary, cells = summarize(hold_raw, hold, hold_labels, fitted, protocol)
    challenger = max(("V3", "V2"), key=lambda model: summary["models"][model]["macro_rho"])
    margin = protocol["selection"]["practical_margin_rho"]
    delta = summary["comparisons_vs_v1"][challenger]["delta"]
    selected = challenger if delta >= margin else "V1"
    selection = {
        "protocol_sha256": digest(args.protocol), "v1_sha256": digest(args.frozen_v1),
        "fitted": fitted, "selected_for_confirmation": selected, "locked_challenger": challenger,
        "exploratory_delta_for_challenger": delta,
        "input_hashes": {name: digest(getattr(args, name)) for name in (
            "calibration_candidates", "calibration_policies", "calibration_features", "calibration_labels",
            "holdout_candidates", "holdout_policies", "holdout_features", "holdout_labels")},
        "calibration_label_counts": {model: dict(Counter(row[f"{model}_tier"] for row in cal_labels)) for model in fitted},
    }
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output / "selection-lock.json", selection, mode="x")
    write_json(args.output / "exploratory-summary.json", summary)
    write_csv(args.output / "calibration-labels.csv", cal_labels)
    write_csv(args.output / "exploratory-labels.csv", hold_labels)
    write_csv(args.output / "exploratory-recipe-agent.csv", cells)
    print(json.dumps({"selected": selected, "challenger": challenger, "delta": delta,
                      "selection_sha256": digest(args.output / "selection-lock.json"),
                      "primary_scores": {model: summary["models"][model]["macro_rho"] for model in MODELS}}, indent=2))


def confirm(args, protocol):
    selection = load_json(args.selection_lock)
    require(selection["protocol_sha256"] == digest(args.protocol), "Confirmation protocol changed after selection")
    for name in ("calibration_candidates", "holdout_candidates"):
        require(selection["input_hashes"][name] == digest(getattr(args, name)), f"{name} no longer matches the selection lock")
    raw, cohort = load_cohort(args.confirmation_candidates, args.confirmation_features, protocol, protocol["confirmation_n"])
    check_cohort_seed_disjoint(read_csv(args.calibration_candidates), read_csv(args.holdout_candidates), raw)
    spec = protocol["confirmation"]
    for stratum, seed in zip(protocol["strata"], spec["seed_starts_in_stratum_order"]):
        used = [int(row["seed"]) for row in raw if row["stratum"] == stratum]
        require(len(used) <= spec["candidate_cap_per_stratum"] and used == list(range(seed, seed + len(used))),
                f"Confirmation seed stream departed from protocol in {stratum}")

    fitted = selection["fitted"]
    labels = scored_rows(cohort, fitted)
    summary, cells = summarize(raw, cohort, labels, fitted, protocol)
    margin = protocol["selection"]["practical_margin_rho"]
    challenger = selection["locked_challenger"]
    changes = summary["comparisons_vs_v1"]
    effect = changes[challenger]
    supported = effect["ci95"][0] > 0 and effect["delta"] >= margin
    omission_supported = changes["V3"]["ci95"][0] > -margin
    chain_supported = changes["V3"]["ci95"][1] < -margin
    verdict = challenger if supported else "V3" if challenger == "V3" and omission_supported else "V1"
    summary["decision"] = {"frozen_selection": selection["selected_for_confirmation"], "challenger": challenger,
                           "superiority_supported": supported, "omission_noninferiority_supported": omission_supported,
                           "chain_feature_retention_evidence": chain_supported, "recommended_baseline": verdict,
                           "selection_lock_sha256": digest(args.selection_lock)}
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output / "confirmation-summary.json", summary)
    write_csv(args.output / "confirmation-labels.csv", labels)
    write_csv(args.output / "confirmation-recipe-agent.csv", cells)
    print(json.dumps({"decision": summary["decision"], "deltas": changes,
                      "primary_scores": {model: summary["models"][model]["macro_rho"] for model in MODELS}}, indent=2))


def main():
    parser = argparse.ArgumentParser(description="Compare candidate cascade components without changing the V1 baseline")
    parser.add_argument("--protocol", type=Path, required=True)
    commands = parser.add_subparsers(dest="operation", required=True)
    for command, run, paths in (
        ("explore", explore, ("frozen_v1", "calibration_candidates", "calibration_policies", "calibration_features",
                              "calibration_labels", "holdout_candidates", "holdout_policies", "holdout_features", "holdout_labels", "output")),
        ("confirm", confirm, ("selection_lock", "calibration_candidates", "holdout_candidates",
                              "confirmation_candidates", "confirmation_features", "output")),
    ):
        stage = commands.add_parser(command)
        for name in paths:
            stage.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
        stage.set_defaults(run=run)
    args = parser.parse_args()
    args.run(args, load_json(args.protocol))


if __name__ == "__main__":
    main()
