import argparse
import hashlib
import json
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from check_holdout import read_table


def record_candidate(row, board, seen_seeds, seen_boards):
    seed = int(row["seed"])
    board_hash = hashlib.sha256(board.read_bytes()).digest()
    if seed in seen_seeds or board_hash in seen_boards:
        raise ValueError(f"Repeated seed or identical board: {row['id']}")
    seen_seeds.add(seed)
    seen_boards.add(board_hash)


def computed_tier(row, standard):
    ease = sum(float(row[field]) for field in standard["features"]) / len(standard["features"])
    assert abs(ease - float(row["ease"])) <= 1e-10
    lower, upper = standard["ease_cutoffs"]
    return "hard" if ease < lower else "normal" if ease < upper else "easy"


def main():
    parser = argparse.ArgumentParser(description="Verify the separately generated production pool")
    parser.add_argument("production", type=Path)
    parser.add_argument("holdout", type=Path)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("standard", type=Path)
    parser.add_argument("verifier", type=Path)
    parser.add_argument("--mixed", type=Path)
    args = parser.parse_args()
    standard_bytes = args.standard.read_bytes()
    standard = json.loads(standard_bytes)
    standard_hash = hashlib.sha256(standard_bytes).hexdigest()
    seen_seeds, seen_boards = set(), set()
    selected = []
    for dataset in (args.calibration, args.holdout):
        for row in read_table(dataset / "candidates.csv"):
            record_candidate(row, dataset / "boards" / f"{row['id']}.txt", seen_seeds, seen_boards)

    for tier in ("easy", "normal", "hard"):
        directory = args.production / tier
        plans = standard["production_plan"][tier]
        summary = json.loads((directory / "production-summary.json").read_bytes())
        attempts = read_table(directory / "production-attempts.csv")
        pool = read_table(directory / "pool.csv")
        assert summary["standard_sha256"] == standard_hash
        assert summary["target"] == tier and summary["plan"] == plans
        assert len(attempts) == summary["candidates"] <= plans["candidate_cap"]
        assert [int(row["seed"]) for row in attempts] == list(range(plans["first_seed"], plans["first_seed"] + len(attempts)))
        assert all(row["stratum"] == plans["stratum"] for row in attempts)

        matching_ids = [row["id"] for row in attempts if row["target_match"] == "1"]
        admitted_ids = [row["id"] for row in attempts if row["admitted"] == "1"]
        assert matching_ids[:plans["target_count"]] == admitted_ids == [row["id"] for row in pool]
        assert len(pool) == summary["admitted"] == plans["target_count"]

        for row in attempts:
            chunk_dir = directory / "chunks" / f"{int(row['chunk']):03d}"
            record_candidate(row, chunk_dir / "boards" / f"{row['id']}.txt", seen_seeds, seen_boards)

        for row in pool:
            assert row["tier"] == row["target_tier"] == computed_tier(row, standard) == tier
            selected.append((directory, row))
        print(f"{tier}: {len(pool)} admitted of {len(attempts)} attempted; plan and tier check passed")

    if args.mixed:
        directory = args.mixed
        summary = json.loads((directory / "production-summary.json").read_bytes())
        plan = summary["plan"]
        quota = plan["target_per_tier"]
        attempts = read_table(directory / "production-attempts.csv")
        pool = read_table(directory / "pool.csv")
        assert summary["target"] == "mixed" and summary["pool_full"]
        assert summary["standard_sha256"] == standard_hash == plan["standard_sha256"]
        assert len(attempts) == summary["candidates"] <= plan["candidate_cap"]
        assert len(pool) == summary["admitted"] == quota * 3
        assert [int(row["seed"]) for row in attempts] == list(range(plan["first_seed"], plan["first_seed"] + len(attempts)))
        assert all(row["stratum"] == plan["stratum"] for row in attempts)
        for row in attempts:
            chunk_dir = directory / "chunks" / f"{int(row['chunk']):03d}"
            record_candidate(row, chunk_dir / "boards" / f"{row['id']}.txt", seen_seeds, seen_boards)
        for tier in ("easy", "normal", "hard"):
            matching_ids = [row["id"] for row in attempts if row["scored_tier"] == tier]
            admitted_ids = [row["id"] for row in attempts if row["scored_tier"] == tier and row["admitted"] == "1"]
            entries = [row for row in pool if row["tier"] == tier]
            assert matching_ids[:quota] == admitted_ids == [row["id"] for row in entries]
            assert len(entries) == quota
        for row in pool:
            assert row["tier"] == row["target_tier"] == computed_tier(row, standard)
            selected.append((directory, row))
        print(f"mixed: {len(pool)} admitted of {len(attempts)} attempted; plan and tier check passed")

    def verify(entry):
        directory, row = entry
        output = subprocess.run([
            args.verifier, "verify", directory / row["board"], row["goal"], row["moves"],
            directory / row["certificate"],
        ], capture_output=True, text=True, check=True)
        assert output.stdout.strip() == "PASS", row["id"]

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(verify, selected))
    print(f"Verified {len(selected)} admitted certificates. {len(seen_seeds)} seeds and boards unique across all cohorts.")


if __name__ == "__main__":
    main()
