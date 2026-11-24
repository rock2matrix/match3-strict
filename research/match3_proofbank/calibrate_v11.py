import argparse
import statistics
import sys
from collections import Counter
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "match3_acr"))
from m5_calibration import (  # noqa: E402
    RECIPES, TIERS, compare_cells, load_controller, make_scores, midpoints, read_csv, read_json, sha, write_csv, write_json,
)


def verified_ids(path):
    return {row["id"] for row in read_csv(path) if row["cert_verified"] == "1" and row["verdict"] == "WIN_CERTIFIED"}


def calibrate(args):
    protocol = read_json(args.protocol)
    assert sha(args.witness_standard) == protocol["production_model_sha256"]
    prior_version = read_json(args.prior_acr)
    assert prior_version["version"] == "strict-random-acr-v1-study"
    boards, metrics = load_controller(args.summary, 100)
    assert set(boards) == verified_ids(args.candidates)
    population = sorted(statistics.mean(metrics[id, "Random"].values()) for id in boards)
    cuts = midpoints(population)
    assert cuts[0] < cuts[1]
    scored = make_scores(boards, metrics, cuts)
    value = {"version": "strict-random-acr-v1.1-targeted-study", "controller": "Adaptive",
             "proof_bank_profile": "targeted-v1.1", "protocol_sha256": sha(args.protocol),
             "production_v1_sha256": sha(args.witness_standard), "prior_acr_sha256": sha(args.prior_acr),
             "prior_acr_cuts": prior_version["cuts"], "calibration_source_sha256": sha(args.summary),
             "calibration_seeds": sorted(int(board["seed"]) for board in boards.values()), "cuts": cuts,
             "calibration_levels": len(scored), "tier_counts": dict(Counter(row["tier"] for row in scored)),
             "rule": "equal arithmetic mean of Random spare-step, Q+3, and chain occurrence under expanded ACR"}
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output / "acr-v11-standard-lock.json", value, "x")
    write_csv(args.output / "calibration-levels.csv", scored)
    print(f"Calibrated v1.1 cutoffs {cuts}; lock SHA {sha(args.output / 'acr-v11-standard-lock.json')}")


def validate(args):
    fitted = read_json(args.lock)
    assert sha(args.protocol) == fitted["protocol_sha256"]
    boards, metrics = load_controller(args.summary, 200)
    assert set(boards) == verified_ids(args.candidates)
    evaluation_seeds = {int(board["seed"]) for board in boards.values()}
    main_experiment = {int(row["seed"]) for row in read_csv(args.benchmark_candidates)}
    assert not evaluation_seeds.intersection(fitted["calibration_seeds"]) and not evaluation_seeds.intersection(main_experiment)
    witness = {row["id"]: row for row in read_csv(args.witness_levels)}
    acr10 = {row["id"]: row for row in read_csv(args.prior_levels)}
    assert set(witness) == set(acr10) == set(boards)
    scored = make_scores(boards, metrics, fitted["cuts"])
    for row in scored:
        id = row["id"]
        row["v1_witness_tier"] = witness[id]["tier"]
        row["acr10_tier"] = acr10[id]["tier"]
        row["v1_witness_ease"] = float(witness[id]["ease"])
        row["acr10_ease"] = float(acr10[id]["ease"])

    cells = compare_cells(scored, metrics, witness)
    eligible = [row for row in cells if row["agent"] != "Random" and row["rho_acr_score"] is not None]
    coverage = {recipe: {tier: sum(row["recipe"] == recipe and row["tier"] == tier for row in scored)
                         for tier in TIERS} for recipe in RECIPES}
    value = {"new_n": len(scored), "locked_model_sha256": sha(args.lock), "validation_source_sha256": sha(args.summary),
             "by_recipe": coverage, "total": dict(Counter(row["tier"] for row in scored)),
             "has_all_tiers_each_recipe": all(all(counts.values()) for counts in coverage.values()),
             "tier_changes_vs_witness": sum(row["tier"] != row["v1_witness_tier"] for row in scored),
             "tier_changes_vs_acr10": sum(row["tier"] != row["acr10_tier"] for row in scored),
             "cross_agent_macro": statistics.mean(row["rho_acr_score"] for row in eligible), "eligible_cells": len(eligible)}
    write_json(args.output / "validation-summary.json", value)
    write_csv(args.output / "validation-levels.csv", scored)
    write_csv(args.output / "validation-recipe-agent.csv", cells)
    print(value)


def main():
    parser = argparse.ArgumentParser(description="Freeze separately calibrated v1.1 difficulty bands")
    parser.add_argument("--protocol", type=Path, default=HERE / "protocol.json")
    commands = parser.add_subparsers(dest="operation", required=True)
    for name, run, fields in (
        ("calibrate", calibrate, ("summary", "candidates", "witness_standard", "prior_acr", "output")),
        ("validate", validate, ("lock", "summary", "candidates", "benchmark_candidates", "witness_levels", "prior_levels", "output")),
    ):
        cmd = commands.add_parser(name)
        for field in fields:
            cmd.add_argument(field, type=Path)
        cmd.set_defaults(run=run)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
