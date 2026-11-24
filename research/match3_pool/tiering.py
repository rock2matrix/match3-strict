import argparse
import csv
import hashlib
import json
import statistics
import subprocess
import time
from collections import Counter, defaultdict
from pathlib import Path


TIERS = ("hard", "normal", "easy")
STRATA = ("g6-m3", "g10-m4", "g8-m2", "g10-m3")
AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
FEATURES = ("exact_early", "exact_challenge", "exact_chain")
TARGET_SEEDS = {"hard": 3200000, "normal": 1300000, "easy": 1200000}
TARGET_CAPS = {"hard": 3000, "normal": 300, "easy": 220}
COMMIT = "71aac8aab2e3fae2fa798e075f0f7d52b1220754"


def read_rows(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def write_rows(path, rows, fields=None):
    if fields is None:
        fields = list(rows[0])
    with Path(path).open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_json(path, value, mode="w"):
    with Path(path).open(mode, encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, ensure_ascii=False, allow_nan=False)
        handle.write("\n")


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def index_unique(rows, key):
    by_key = {}
    for row in rows:
        value = key(row)
        if value in by_key:
            raise ValueError(f"Duplicate key {value}")
        by_key[value] = row
    return by_key


def primary(row):
    return statistics.mean(float(row[field]) for field in FEATURES)


def alt_score(row, spec):
    return sum(float(row[field]) * weight for field, weight in zip(spec["fields"], spec["weights"]))


def classify(value, cutoffs):
    if value < cutoffs[0]:
        return "hard"
    if value < cutoffs[1]:
        return "normal"
    return "easy"


def tertiles(values):
    ordered = sorted(values)
    return [statistics.mean((ordered[index - 1], ordered[index])) for index in (33, 66)]


def verified(row):
    passed = row["cert_verified"] == "1"
    if passed != (row["verdict"] == "WIN_CERTIFIED"):
        raise ValueError(f"Candidate certificate status mismatch: {row['id']}")
    return passed


def build_labels(candidates, policies, cutoffs):
    index_unique(candidates, lambda row: row["id"])
    index_unique(candidates, lambda row: row["seed"])
    by_id = index_unique((row for row in policies if row["agent"] == "Random"), lambda row: row["id"])
    if set(by_id) != {row["id"] for row in candidates if verified(row)}:
        raise ValueError("Random score rows must match exactly the verified candidates")

    result = []
    for row in candidates:
        if row["id"] not in by_id:
            continue
        policy = by_id[row["id"]]
        values = tuple(float(policy[field]) for field in FEATURES)
        if not all(0 <= value <= 1.00000000001 for value in values):
            raise ValueError(f"Probability out of bounds: {row['id']}")
        ease = primary(policy)
        result.append({
            **{key: row[key] for key in ("id", "seed", "stratum", "goal", "moves")},
            **dict(zip(FEATURES, values)),
            "ease": ease,
            "difficulty": 100 * (1 - ease),
            "tier": classify(ease, cutoffs),
            "boundary_distance": min(abs(ease - limit) for limit in cutoffs),
        })
    return result


def calibrate(args):
    candidates, policies = read_rows(args.candidates), read_rows(args.policies)
    random_rows = [row for row in policies if row["agent"] == "Random"]
    if len(random_rows) != 100:
        raise ValueError("Expected the 100-board calibration cohort")
    exact = tertiles([primary(row) for row in random_rows])
    cutoffs = [round(value, 3) for value in exact]
    labels = build_labels(candidates, policies, cutoffs)
    counts = Counter(row["tier"] for row in labels)
    if counts != {"hard": 33, "normal": 33, "easy": 34}:
        raise ValueError("Rounding changed calibration tier counts")

    alternatives = {
        "weight_40_40_20": {"fields": FEATURES, "weights": (0.4, 0.4, 0.2)},
        "surplus_plus_1": {"fields": ("exact_early", "exact_surplus_1", "exact_chain"), "weights": (1/3,) * 3},
        "surplus_plus_6": {"fields": ("exact_early", "exact_surplus_6", "exact_chain"), "weights": (1/3,) * 3},
    }
    for spec in alternatives.values():
        spec["cutoffs"] = tertiles([alt_score(row, spec) for row in random_rows])

    route_stats = {}
    for stratum in STRATA:
        rows = [row for row in candidates if row["stratum"] == stratum]
        total_ms = sum(float(row["solve_us"]) + float(row["verify_us"]) for row in rows) / 1000
        hits = Counter(row["tier"] for row in labels if row["stratum"] == stratum)
        route_stats[stratum] = {
            "candidates": len(rows), "hits": {tier: hits[tier] for tier in TIERS},
            "solver_plus_verify_ms": total_ms,
            "solver_plus_verify_ms_per_hit": {tier: total_ms / hits[tier] for tier in TIERS},
        }
    plan = {}
    for tier in TIERS:
        best = min(route_stats, key=lambda stratum: route_stats[stratum]["solver_plus_verify_ms_per_hit"][tier])
        plan[tier] = {"stratum": best, "first_seed": TARGET_SEEDS[tier],
                      "candidate_cap": TARGET_CAPS[tier], "target_count": 20, "certificates_per_chunk": 8}

    value = {
        "standard": "strict-random-tier-v1", "solver_commit": COMMIT,
        "calibration_files_sha256": {"candidates.csv": digest(args.candidates), "policies.csv": digest(args.policies)},
        "calibration_seed_minmax": [min(map(lambda row: int(row["seed"]), candidates)),
                                    max(map(lambda row: int(row["seed"]), candidates))],
        "calibration_seeds": sorted(int(row["seed"]) for row in candidates),
        "calibration_tier_counts": dict(counts), "calibration_routes": route_stats,
        "features": FEATURES, "reference_agent": "Random", "weights": [1/3] * 3,
        "ease_cutoffs": cutoffs, "unrounded_calibration_cutoffs": exact, "alternatives": alternatives,
        "holdout_plan": {"first_seeds": [1100000, 2100000, 3100000, 4100000],
                         "certified_per_stratum": 50, "candidate_cap_per_stratum": 4000},
        "production_plan": plan,
    }
    write_json(args.standard, value, mode="x")
    write_rows(args.labels, labels)
    print(json.dumps({key: value[key] for key in ("ease_cutoffs", "calibration_tier_counts", "production_plan")}, indent=2))
    print(f"Frozen SHA-256 {digest(args.standard)}")


def summarize_labels(candidates, labels, policies, standard):
    result = {"candidates": len(candidates), "verified": len(labels), "strata": {}, "tiers": {}}
    by_id = index_unique(policies, lambda row: (row["id"], row["agent"]))
    for stratum in STRATA:
        batch = [row for row in candidates if row["stratum"] == stratum]
        grouped_labels = [row for row in labels if row["stratum"] == stratum]
        by_status = Counter(row["verdict"] for row in batch)
        counts = Counter(row["tier"] for row in grouped_labels)
        result["strata"][stratum] = {
            "candidates": len(batch), "verified": len(grouped_labels), "verdicts": dict(by_status),
            "tiers": {tier: counts[tier] for tier in TIERS},
            "solver_plus_verify_ms": sum(float(row["solve_us"]) + float(row["verify_us"]) for row in batch) / 1000,
        }
    for tier in TIERS:
        rows = [row for row in labels if row["tier"] == tier]
        stats = {"count": len(rows)}
        if rows:
            stats["mean_ease"] = statistics.mean(row["ease"] for row in rows)
            stats["mean_difficulty"] = statistics.mean(row["difficulty"] for row in rows)
            stats["agents"] = {
                agent: {field: statistics.mean(float(by_id[row["id"], agent][field]) for row in rows)
                        for field in FEATURES}
                for agent in AGENTS if all((row["id"], agent) in by_id for row in rows)
            }
            stats["near_cutoff_1pt"] = sum(row["boundary_distance"] <= 0.01 for row in rows)
        result["tiers"][tier] = stats
    result["sensitivity"] = {}
    for name, spec in standard["alternatives"].items():
        if not all(field in policies[0] for field in spec["fields"]):
            continue
        transitions = Counter((row["tier"], classify(alt_score(by_id[row["id"], "Random"], spec), spec["cutoffs"]))
                              for row in labels)
        result["sensitivity"][name] = {
            "same": sum(count for (before, after), count in transitions.items() if before == after),
            "two_tiers_off": transitions["hard", "easy"] + transitions["easy", "hard"],
            "transitions": {f"{a}->{b}": n for (a, b), n in sorted(transitions.items())},
        }
    return result


def validate(args):
    standard = load_json(args.standard)
    plan = standard["holdout_plan"]
    candidates, policies = read_rows(args.candidates), read_rows(args.policies)
    if set(standard["calibration_seeds"]) & {int(row["seed"]) for row in candidates}:
        raise ValueError("Holdout seeds overlap calibration")
    index_unique(policies, lambda row: (row["id"], row["agent"]))
    labels = build_labels(candidates, policies, standard["ease_cutoffs"])
    for stratum, start in zip(STRATA, plan["first_seeds"]):
        seeds = [int(row["seed"]) for row in candidates if row["stratum"] == stratum]
        if seeds != list(range(start, start + len(seeds))) or len(seeds) > plan["candidate_cap_per_stratum"]:
            raise ValueError(f"Holdout candidate stream deviated from frozen {stratum} plan")
        count = sum(row["stratum"] == stratum for row in labels)
        if count != plan["certified_per_stratum"]:
            raise ValueError(f"Holdout {stratum} incomplete: {count} verified")
    if len(labels) * len(AGENTS) != len(policies):
        raise ValueError("Holdout must contain all four policy rows per verified level")

    args.output.mkdir(parents=True, exist_ok=True)
    write_rows(args.output / "holdout-levels.csv", labels)
    result = summarize_labels(candidates, labels, policies, standard)
    result["standard_sha256"] = digest(args.standard)
    write_json(args.output / "holdout-summary.json", result)
    print(f"Holdout: {len(candidates)} candidates, {len(labels)} verified, "
          + ", ".join(f"{tier}={result['tiers'][tier]['count']}" for tier in TIERS))


def fill(args):
    standard = load_json(args.standard)
    lane = standard["production_plan"][args.tier]
    args.output.mkdir(parents=True, exist_ok=False)
    all_attempts = []
    selected = []
    chunks = []
    score_us = 0.0
    verify_us = 0.0
    verified_count = 0
    seen_seed = set()
    cap = lane["candidate_cap"]
    quota = lane["target_count"]
    seed = lane["first_seed"]
    started = time.perf_counter()
    while len(selected) < quota and len(all_attempts) < cap:
        chunk_number = len(chunks) + 1
        chunk_dir = args.output / "chunks" / f"{chunk_number:03d}"
        chunk_dir.parent.mkdir(exist_ok=True)
        chunk_started = time.perf_counter()
        generation = subprocess.run([args.generator, str(chunk_dir), str(lane["certificates_per_chunk"]),
                                     str(cap - len(all_attempts)), lane["stratum"], str(seed)], check=False)
        if generation.returncode not in (0, 2):
            raise RuntimeError(f"Candidate generation failed ({generation.returncode})")
        batch = read_rows(chunk_dir / "candidates.csv")
        batch_seeds = [int(row["seed"]) for row in batch]
        if not batch or batch_seeds != list(range(seed, seed + len(batch))) or seen_seed.intersection(batch_seeds):
            raise ValueError("Batch seeds deviate from frozen plan")
        seen_seed.update(batch_seeds)
        seed = batch_seeds[-1] + 1
        verified_batch = sum(verified(row) for row in batch)
        verified_count += verified_batch
        if verified_batch:
            score_path = chunk_dir / "scores.csv"
            subprocess.run([args.scorer, str(chunk_dir), str(score_path), "random-minimal"], check=True)
            scored = read_rows(score_path)
            labels = {row["id"]: row for row in build_labels(batch, scored, standard["ease_cutoffs"])}
            score_us += sum(float(row["score_us"]) for row in scored)
            verify_us += sum(float(row["verify_us"]) for row in scored)
        else:
            labels = {}
        hits = 0
        for candidate in batch:
            label = labels.get(candidate["id"])
            match = label is not None and label["tier"] == args.tier
            selected_now = match and len(selected) < quota
            hits += match
            if selected_now:
                selected.append({**label, "target_tier": args.tier, "chunk": chunk_number,
                                 "board": f"chunks/{chunk_number:03d}/boards/{label['id']}.txt",
                                 "certificate": f"chunks/{chunk_number:03d}/certificates/{label['id']}.m3cert"})
            all_attempts.append({**candidate, "chunk": chunk_number,
                                 "scored_tier": label["tier"] if label else "",
                                 "ease": label["ease"] if label else "", "target_match": int(match),
                                 "admitted": int(selected_now)})
        chunks.append({"chunk": chunk_number, "candidates": len(batch), "verified": verified_batch,
                       "matches": hits, "wall_s": time.perf_counter() - chunk_started})
        print(f"{args.tier}: {len(selected)}/{quota} admitted; {len(all_attempts)} candidates", flush=True)
    assert not seen_seed.intersection(standard["calibration_seeds"])
    write_rows(args.output / "production-attempts.csv", all_attempts)
    pool_fields = list(selected[0]) if selected else ["id", "seed", "stratum", "goal", "moves", *FEATURES,
                    "ease", "difficulty", "tier", "boundary_distance", "target_tier", "chunk", "board", "certificate"]
    write_rows(args.output / "pool.csv", selected, pool_fields)
    matches = sum(row["target_match"] for row in all_attempts)
    summary = {"target": args.tier, "plan": lane, "admitted": len(selected), "matching": matches,
               "candidates": len(all_attempts), "verified": verified_count,
               "statuses": dict(Counter(row["verdict"] for row in all_attempts)),
               "all_scored_tiers": dict(Counter(row["scored_tier"] for row in all_attempts if row["scored_tier"])),
               "chunks": chunks, "wall_s": time.perf_counter() - started,
               "solver_s": sum(float(row["solve_us"]) for row in all_attempts) / 1e6,
               "generation_verify_s": sum(float(row["verify_us"]) for row in all_attempts) / 1e6,
               "scorer_s": score_us / 1e6, "scorer_verify_s": verify_us / 1e6,
               "standard_sha256": digest(args.standard), "last_seed": seed - 1}
    write_json(args.output / "production-summary.json", summary)


def main():
    parser = argparse.ArgumentParser(description="Calibrate, hold out and fill a fixed Strict difficulty pool")
    commands = parser.add_subparsers(dest="command", required=True)
    for name, method, fields in (
        ("freeze", calibrate, ("candidates", "policies", "standard", "labels")),
        ("validate", validate, ("candidates", "policies", "standard", "output")),
        ("fill", fill, ("standard", "generator", "scorer", "output")),
    ):
        cmd = commands.add_parser(name)
        for field in fields:
            cmd.add_argument(field, type=Path)
        if name == "fill":
            cmd.add_argument("tier", choices=TIERS)
        cmd.set_defaults(method=method)
    args = parser.parse_args()
    args.method(args)


if __name__ == "__main__":
    main()
