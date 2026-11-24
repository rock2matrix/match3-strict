"""Fill all three fixed tiers from one sequential g6-m3 candidate stream."""

import argparse
import statistics
import subprocess
import time
from collections import Counter
from pathlib import Path

from tiering import FEATURES, TIERS, build_labels, digest, load_json, read_rows, verified, write_json, write_rows


def freeze(path, standard_path, standard, standard_hash):
    if standard["ease_cutoffs"] != [0.554, 0.651]:
        raise ValueError("The supplemental plan requires the frozen v1 cutoffs")
    if standard_hash != digest(standard_path):
        raise ValueError("The tier standard changed while being read")
    write_json(path, {
        "experiment": "exploratory_single_stream_after_primary_validation",
        "standard_sha256": standard_hash,
        "stratum": "g6-m3",
        "first_seed": 1400000,
        "candidate_cap": 250,
        "target_per_tier": 20,
        "certificates_per_chunk": 8,
        "admission": "First available match for each tier until its quota is full, in increasing seed order",
        "note": "Recipe and plan were chosen after reviewing the primary holdout and production results, before scoring this disjoint seed block.",
    }, mode="x")


def check_seeds(plan, standard):
    first = plan["first_seed"]
    end = first + plan["candidate_cap"]
    if any(first <= seed < end for seed in standard["calibration_seeds"]):
        raise ValueError("Overlapping calibration seeds")
    holdout = standard["holdout_plan"]
    occupied = [
        (seed, seed + holdout["candidate_cap_per_stratum"])
        for seed in holdout["first_seeds"]
    ] + [
        (lane["first_seed"], lane["first_seed"] + lane["candidate_cap"])
        for lane in standard["production_plan"].values()
    ]
    if any(first < stop and start < end for start, stop in occupied):
        raise ValueError("Overlapping holdout or dedicated-lane seeds")


def fill(args, standard, plan):
    check_seeds(plan, standard)
    args.output.mkdir(parents=True, exist_ok=False)
    chunk_root = args.output / "chunks"
    chunk_root.mkdir()
    started = time.perf_counter()
    attempts = []
    pool = []
    chunks = []
    hits = Counter({tier: 0 for tier in TIERS})
    admitted = hits.copy()
    verified_count = 0
    scorer_s = scorer_verify_s = 0.0
    quota = plan["target_per_tier"]
    cap = plan["candidate_cap"]
    seed = plan["first_seed"]
    while any(admitted[tier] < quota for tier in TIERS) and len(attempts) < cap:
        number = len(chunks) + 1
        chunk_dir = chunk_root / f"{number:03d}"
        chunk_started = time.perf_counter()
        generated = subprocess.run([
            args.generator, chunk_dir, str(plan["certificates_per_chunk"]), str(cap - len(attempts)),
            plan["stratum"], str(seed),
        ], check=False)
        if generated.returncode not in (0, 2):
            raise RuntimeError(f"Generation exited {generated.returncode}")
        candidates = read_rows(chunk_dir / "candidates.csv")
        if not candidates or [int(row["seed"]) for row in candidates] != list(range(seed, seed + len(candidates))):
            raise ValueError("Candidate seed sequence diverged from the saved plan")
        seed += len(candidates)
        certified = sum(verified(row) for row in candidates)
        verified_count += certified
        labels = {}
        if certified:
            scores_path = chunk_dir / "scores.csv"
            subprocess.run([args.scorer, chunk_dir, scores_path, "random-minimal"], check=True)
            scores = read_rows(scores_path)
            labels = {row["id"]: row for row in build_labels(candidates, scores, standard["ease_cutoffs"])}
            scorer_s += sum(float(row["score_us"]) for row in scores) / 1e6
            scorer_verify_s += sum(float(row["verify_us"]) for row in scores) / 1e6
        chunk_hits = Counter({tier: 0 for tier in TIERS})
        chunk_admitted = chunk_hits.copy()
        for row in candidates:
            label = labels.get(row["id"])
            tier = label["tier"] if label else ""
            selected = bool(tier) and admitted[tier] < quota
            if tier:
                hits[tier] += 1
                chunk_hits[tier] += 1
            if selected:
                admitted[tier] += 1
                chunk_admitted[tier] += 1
                pool.append({
                    **label, "target_tier": tier, "chunk": number,
                    "board": f"chunks/{number:03d}/boards/{row['id']}.txt",
                    "certificate": f"chunks/{number:03d}/certificates/{row['id']}.m3cert",
                })
            attempts.append({
                **row, "chunk": number, "scored_tier": tier, "ease": label["ease"] if label else "",
                "target_match": int(bool(tier)), "admitted": int(selected),
            })
        chunks.append({
            "chunk": number, "candidates": len(candidates), "verified": certified, "matches": dict(chunk_hits),
            "admitted": dict(chunk_admitted), "wall_s": time.perf_counter() - chunk_started,
        })
        print(f"mixed {len(attempts)} candidates, {verified_count} verified, admitted {dict(admitted)}", flush=True)

    if sum(hits.values()) != verified_count or sum(admitted.values()) != len(pool):
        raise ValueError("Final counts disagree with candidate and pool rows")
    if len(attempts) > cap or any(value > quota for value in admitted.values()):
        raise ValueError("Final counts exceed the plan")
    write_rows(args.output / "production-attempts.csv", attempts)
    fields = ["id", "seed", "stratum", "goal", "moves", *FEATURES, "ease", "difficulty", "tier",
              "boundary_distance", "target_tier", "chunk", "board", "certificate"]
    write_rows(args.output / "pool.csv", pool, fields)
    solve_us = [float(row["solve_us"]) for row in attempts]
    generation_verify_us = [float(row["verify_us"]) for row in attempts]
    completion_index = max((i for i, row in enumerate(attempts, 1) if row["admitted"]), default=0)
    summary = {
        "target": "mixed", "plan": plan, "admitted": len(pool), "admitted_per_tier": dict(admitted),
        "matching": verified_count, "candidates": len(attempts), "verified": verified_count,
        "statuses": dict(Counter(row["verdict"] for row in attempts)), "all_scored_tiers": dict(hits),
        "pool_full": all(admitted[tier] == quota for tier in TIERS),
        "raw_candidates_through_last_admission": completion_index,
        "last_seed": seed - 1, "chunks": chunks, "wall_s": time.perf_counter() - started,
        "solver_s": sum(solve_us) / 1e6, "median_solver_us": statistics.median(solve_us),
        "generation_verify_s": sum(generation_verify_us) / 1e6, "scorer_s": scorer_s,
        "scorer_verify_s": scorer_verify_s, "standard_sha256": plan["standard_sha256"],
        "mixed_plan_sha256": digest(args.plan),
    }
    write_json(args.output / "production-summary.json", summary)
    return 0 if summary["pool_full"] else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("standard", "plan", "generator", "scorer", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--freeze", action="store_true", help="create the plan once before generating")
    args = parser.parse_args()
    standard_hash = digest(args.standard)
    standard = load_json(args.standard)
    if args.freeze:
        freeze(args.plan, args.standard, standard, standard_hash)
    plan = load_json(args.plan)
    if plan["standard_sha256"] != standard_hash or digest(args.standard) != standard_hash:
        raise ValueError("The tier standard does not match the saved plan")
    return fill(args, standard, plan)


if __name__ == "__main__":
    raise SystemExit(main())
