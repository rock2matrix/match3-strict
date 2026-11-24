import argparse
import csv
import hashlib
import json
import statistics
import sys
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "match3_ablation"))
from ablation import association, label, midpoints, rank_vector  # noqa: E402


AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
RECIPES = ("g6-m3", "g10-m4", "g8-m2", "g10-m3")
TIERS = ("hard", "normal", "easy")
ENDPOINTS = ("early", "challenge")


def read_csv(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, data, mode="w"):
    with path.open(mode, encoding="utf-8") as stream:
        json.dump(data, stream, indent=2, ensure_ascii=False, allow_nan=False)
        stream.write("\n")


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def load_controller(path, expected_boards):
    selected = [row for row in read_csv(path) if row["controller"] in ("Adaptive", "ACR")]
    metrics, boards = {}, {}
    for row in selected:
        id, agent = row["id"], row["agent"]
        key = (id, agent)
        if key in metrics:
            raise ValueError(f"Repeated ACR row {key}")
        n = int(row["episodes"])
        assert n > 0 and n == int(row["wins"]), key
        result = {metric: int(row[metric]) / n for metric in (*ENDPOINTS, "chain")}
        assert all(0 <= value <= 1 for value in result.values()), key
        metrics[key] = result
        board = {field: row[field] for field in ("id", "seed", "recipe", "goal", "moves")}
        assert id not in boards or boards[id] == board, id
        boards[id] = board
    assert len(boards) == expected_boards
    assert Counter(row["recipe"] for row in boards.values()) == {recipe: expected_boards // 4 for recipe in RECIPES}
    assert set(metrics) == {(id, agent) for id in boards for agent in AGENTS}
    return boards, metrics


def make_scores(boards, metrics, cuts):
    scored = []
    for id, board in boards.items():
        prob = metrics[id, "Random"]
        score = statistics.mean(prob.values())
        scored.append({**board, **prob, "ease": score, "tier": label(score, cuts)})
    return scored


def calibrate(args):
    plan = read_json(args.plan)
    assert sha(args.witness_v1) == plan["production_v1_sha256"]
    boards, metrics = load_controller(args.calibration, 100)
    population = sorted(statistics.mean(metrics[board, "Random"].values()) for board in boards)
    cuts = midpoints(population)
    fitted = make_scores(boards, metrics, cuts)
    frozen = {"version": "strict-random-acr-v1-study", "status": "experimental", "controller": "Adaptive",
              "experiment_plan_sha256": sha(args.plan), "witness_v1_sha256": sha(args.witness_v1),
              "calibration_input_sha256": sha(args.calibration), "calibration_n": len(boards),
              "calibration_seeds": sorted(int(row["seed"]) for row in boards.values()),
              "score": "arithmetic mean of Random early/challenge/chain under Adaptive",
              "cuts": cuts, "old_witness_cuts": read_json(args.witness_v1)["ease_cutoffs"],
              "calibration_counts": dict(Counter(row["tier"] for row in fitted))}
    args.output.mkdir(exist_ok=True, parents=True)
    write_json(args.output / "acr-standard-lock.json", frozen, "x")
    write_csv(args.output / "calibration-levels.csv", fitted)
    print(json.dumps({key: frozen[key] for key in ("version", "cuts", "calibration_counts")}, indent=2))


def rho(x, y):
    xs = rank_vector([round(value, 12) for value in x])
    ys = rank_vector([round(value, 12) for value in y])
    return None if ys is None else association(xs, ys)


def compare_cells(records, metrics, witness):
    results = []
    for recipe in RECIPES:
        subset = [row for row in records if row["recipe"] == recipe]
        for agent in AGENTS:
            for endpoint in ENDPOINTS:
                effect = [metrics[row["id"], agent][endpoint] for row in subset]
                results.append({"recipe": recipe, "agent": agent, "endpoint": endpoint, "n": len(subset),
                                "rho_acr_score": rho([row["ease"] for row in subset], effect),
                                "rho_witness_score": rho([float(witness[row["id"]]["ease"]) for row in subset], effect),
                                **{f"n_{tier}": sum(row["tier"] == tier for row in subset) for tier in TIERS},
                                **{f"mean_{tier}": statistics.mean(values) if values else None
                                   for tier in TIERS for values in ([value for row, value in zip(subset, effect) if row["tier"] == tier],)}})
    return results


def validate(args):
    frozen = read_json(args.lock)
    assert frozen["experiment_plan_sha256"] == sha(args.plan)
    boards, metrics = load_controller(args.validation, 200)
    search = {int(row["seed"]) for row in read_csv(args.m3_candidates)}
    validation = {int(row["seed"]) for row in boards.values()}
    assert not validation.intersection(frozen["calibration_seeds"]) and not validation.intersection(search)
    previous = read_csv(args.witness_holdout)
    witness = {entry["id"]: entry for entry in previous}
    assert set(witness) == set(boards) and len(witness) == len(previous)
    scored = make_scores(boards, metrics, frozen["cuts"])
    for level in scored:
        prior = witness[level["id"]]
        level["witness_tier"] = prior["tier"]
        level["tier_if_transplanted_witness_cut"] = label(level["ease"], frozen["old_witness_cuts"])
        level["witness_ease"] = float(prior["ease"])

    distributions = {}
    for recipe in RECIPES:
        selected = [row for row in scored if row["recipe"] == recipe]
        distributions[recipe] = {tier: sum(row["tier"] == tier for row in selected) for tier in TIERS}
    cells = compare_cells(scored, metrics, witness)
    eligible = [row for row in cells if row["agent"] != "Random" and row["rho_acr_score"] is not None]
    result = {"validation_n": len(scored), "lock_sha256": sha(args.lock), "validation_summary_sha256": sha(args.validation),
              "counts": dict(Counter(row["tier"] for row in scored)), "by_recipe": distributions,
              "has_all_tiers_per_recipe": all(all(values.values()) for values in distributions.values()),
              "changed_after_controller_specific_calibration": sum(row["tier"] != row["witness_tier"] for row in scored),
              "changed_if_reusing_witness_cuts": sum(row["tier_if_transplanted_witness_cut"] != row["witness_tier"] for row in scored),
              "rho_within_recipe_vs_witness": {recipe: rho([row["witness_ease"] for row in scored if row["recipe"] == recipe],
                                                           [row["ease"] for row in scored if row["recipe"] == recipe]) for recipe in RECIPES},
              "nonconstant_cross_agent_cells": len(eligible),
              "acr_rank_macro": statistics.mean(row["rho_acr_score"] for row in eligible),
              "witness_rank_for_acr_outcomes_macro": statistics.mean(row["rho_witness_score"] for row in eligible)}
    args.output.mkdir(exist_ok=True, parents=True)
    write_csv(args.output / "validation-levels.csv", scored)
    write_csv(args.output / "validation-recipe-agent.csv", cells)
    write_json(args.output / "validation-summary.json", result)
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description="Calibrate an ACR-tagged model while preserving Witness V1")
    parser.add_argument("--plan", type=Path, default=ROOT / "experiment-plan.json")
    stages = parser.add_subparsers(dest="stage", required=True)
    for name, steps, fields in (
        ("calibrate", calibrate, ("calibration", "witness_v1", "output")),
        ("validate", validate, ("lock", "validation", "m3_candidates", "witness_holdout", "output")),
    ):
        cmd = stages.add_parser(name)
        for field in fields:
            cmd.add_argument(field, type=Path)
        cmd.set_defaults(run=steps)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
