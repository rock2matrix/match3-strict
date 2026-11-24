import argparse
import csv
import hashlib
import statistics
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory


def read_table(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def board_fingerprints(dataset, candidates):
    return {hashlib.sha256((dataset / "boards" / f"{row['id']}.txt").read_bytes()).digest()
            for row in candidates}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("holdout", type=Path)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("verifier", type=Path)
    parser.add_argument("generator", type=Path)
    args = parser.parse_args()
    holdout = read_table(args.holdout / "candidates.csv")
    calibration = read_table(args.calibration / "candidates.csv")
    holdout_selected = [row for row in holdout if row["cert_verified"] == "1"]
    assert len(holdout_selected) == 200
    assert all(row["verdict"] == "WIN_CERTIFIED" for row in holdout_selected)
    assert len({row["seed"] for row in holdout}) == len(holdout)
    assert {row["seed"] for row in holdout}.isdisjoint(row["seed"] for row in calibration)
    holdout_hashes = board_fingerprints(args.holdout, holdout)
    assert len(holdout_hashes) == len(holdout)
    assert holdout_hashes.isdisjoint(board_fingerprints(args.calibration, calibration))

    def verify(row):
        output = subprocess.run([
            args.verifier, "verify", args.holdout / "boards" / f"{row['id']}.txt",
            row["goal"], row["moves"], args.holdout / "certificates" / f"{row['id']}.m3cert",
        ], check=True, capture_output=True, text=True)
        assert output.stdout.strip() == "PASS", row["id"]

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(verify, holdout_selected))

    groups = sorted({row["stratum"] for row in holdout})
    for group in groups:
        subset = [row for row in holdout if row["stratum"] == group]
        hits = Counter(row["verdict"] for row in subset)
        assert hits["WIN_CERTIFIED"] == 50
        assert [int(row["seed"]) for row in subset] == list(range(int(subset[0]["seed"]), int(subset[0]["seed"]) + len(subset)))
        solves = [float(row["solve_us"]) / 1000 for row in subset]
        print(group, len(subset), dict(hits), f"solve median {statistics.median(solves):.2f} ms")

    with TemporaryDirectory() as workspace:
        output = Path(workspace) / "chunk"
        result = subprocess.run([args.generator, output, "2", "1", "g8-m2", "90000001"], check=False)
        assert result.returncode == 2 and len(read_table(output / "candidates.csv")) == 1
    print(f"Verified all {len(holdout_selected)} certificates with CLI, {len(holdout)} unique seeds/boards disjoint from calibration.")


if __name__ == "__main__":
    main()
