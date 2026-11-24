"""Generate and verify a fresh certification cohort. No difficulty is evaluated."""

import argparse
import csv
import hashlib
import json
import subprocess
import tempfile
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


RECIPES = {"g6-m3": 1600000, "g10-m4": 2600000, "g8-m2": 3600000, "g10-m3": 4600000}
QUOTA = 50
CAP = 4000
COMMIT = "71aac8aab2e3fae2fa798e075f0f7d52b1220754"


def csv_contents(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        return reader.fieldnames, list(reader)


def write_csv(path, fields, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprints(dataset, registry):
    _, rows = csv_contents(dataset / "candidates.csv")
    for row in rows:
        seed = int(row["seed"])
        file = dataset / "boards" / f"{row['id']}.txt"
        board = file.read_bytes().split()
        if len(board) != 64:
            raise ValueError(f"Expected 64 cells: {file}")
        fingerprint = hashlib.sha256(b" ".join(board)).digest()
        if seed in registry["seeds"] or fingerprint in registry["boards"]:
            raise ValueError(f"Seed or initial board reused: {file}")
        registry["seeds"].add(seed)
        registry["boards"].add(fingerprint)


def generate_recipe(generator, staging, recipe_seed):
    recipe, first_seed = recipe_seed
    directory = staging / recipe
    command = [str(generator), str(directory), str(QUOTA), str(CAP), recipe, str(first_seed)]
    start = time.perf_counter()
    run = subprocess.run(command, capture_output=True, text=True, check=True)
    duration = time.perf_counter() - start
    print(run.stderr.strip(), flush=True)
    return directory, {"recipe": recipe, "seed_start": first_seed, "parallel_wall_seconds": duration}


def validate_recipe(directory, info, prior_reference):
    fields, rows = csv_contents(directory / "candidates.csv")
    settings_fields, settings = csv_contents(directory / "settings.csv")
    if len(settings) != 1:
        raise ValueError(f"Expected one recipe settings row: {directory}")
    settings = settings[0]
    expected = {**prior_reference[info["recipe"]], "first_seed": str(info["seed_start"])}
    if settings != expected:
        raise ValueError(f"Settings differ from prior held-out recipe: {directory}")
    seeds = [int(row["seed"]) for row in rows]
    if seeds != list(range(info["seed_start"], info["seed_start"] + len(rows))) or len(rows) > CAP:
        raise ValueError(f"Unexpected seed stream: {directory}")
    certified = [row for row in rows if row["cert_verified"] == "1"]
    if len(certified) != QUOTA or rows[-1] != certified[-1] or certified != [
            row for row in rows if row["verdict"] == "WIN_CERTIFIED"]:
        raise ValueError(f"Certification quota or labels mismatch: {directory}")
    if {file.stem for file in (directory / "boards").glob("*.txt")} != {row["id"] for row in rows}:
        raise ValueError(f"Candidate boards mismatch: {directory}")
    if {file.stem for file in (directory / "certificates").glob("*.m3cert")} != {row["id"] for row in certified}:
        raise ValueError(f"Certificate inventory mismatch: {directory}")
    if any(row["stratum"] != settings["stratum"] or row["goal"] != settings["goal"] or
           row["moves"] != settings["moves"] or int(row["candidate_checks"]) > int(settings["max_candidates"]) or
           int(row["refill_nodes"]) > int(settings["max_refill_nodes"]) or
           int(row["states"]) > int(settings["max_states"]) for row in rows):
        raise ValueError(f"Recipe or budget mismatch: {directory}")
    info["seed_last"] = seeds[-1]
    info["candidates"] = len(rows)
    info["verdicts"] = dict(Counter(row["verdict"] for row in rows))
    info["certificate_bytes"] = sum(file.stat().st_size for file in (directory / "certificates").glob("*.m3cert"))
    return fields, rows, settings_fields, settings, certified


def verify_certificates(cli, tasks):
    def verify(task):
        directory, row = task
        identifier = row["id"]
        run = subprocess.run([str(cli), "verify", str(directory / "boards" / f"{identifier}.txt"),
                              row["goal"], row["moves"],
                              str(directory / "certificates" / f"{identifier}.m3cert")],
                             text=True, capture_output=True, check=True)
        if run.stdout.strip() != "PASS":
            raise ValueError(f"Independent replay failed: {identifier}: {run.stdout}")

    with ThreadPoolExecutor(max_workers=4) as thread_pool:
        list(thread_pool.map(verify, tasks))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--verifier", type=Path, required=True)
    parser.add_argument("--previous", type=Path, required=True, help="work root with match3_difficulty and match3_pool")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    inputs = parser.parse_args()
    work = inputs.previous.resolve()
    output = inputs.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    final_dataset = output / "dataset"
    if final_dataset.exists():
        raise FileExistsError(f"Output dataset already exists: {final_dataset}")
    solver = inputs.solver.resolve()
    git = ["git", "-C", str(solver)]
    if subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip() != COMMIT:
        raise ValueError("Solver checkout differs from the frozen commit")
    if subprocess.check_output([*git, "status", "--porcelain"], text=True).strip():
        raise ValueError("Solver checkout has uncommitted changes")
    generator = inputs.generator.resolve()
    verifier = inputs.verifier.resolve()
    source = work / "match3_pool" / "generate.cpp"
    previous_settings_path = work / "match3_pool" / "dataset" / "settings.csv"
    previous_settings = {row["stratum"]: row for row in csv_contents(previous_settings_path)[1]}
    protocol = {"solver_commit": COMMIT, "generation_source": "match3_pool/generate.cpp",
                "generation_source_sha256": sha256(source), "generator_binary_sha256": sha256(generator),
                "cli_binary_sha256": sha256(verifier), "old_holdout_settings_sha256": sha256(previous_settings_path),
                "recipe_seeds": RECIPES, "certified_quota_per_recipe": QUOTA, "candidate_limit_per_recipe": CAP,
                "generator_cli_arguments": ["OUTPUT_RECIPE_DIR", QUOTA, CAP, "RECIPE", "FIRST_SEED"],
                "difficulty_evaluated": False}
    with (output / "generation-plan.json").open("x", encoding="utf-8") as stream:
        json.dump(protocol, stream, indent=2)
        stream.write("\n")

    started = time.perf_counter()
    with tempfile.TemporaryDirectory(dir=output, prefix="generate-") as staging_name:
        staging = Path(staging_name)
        with ThreadPoolExecutor(max_workers=len(RECIPES)) as thread_pool:
            generated = list(thread_pool.map(lambda entry: generate_recipe(generator, staging, entry), RECIPES.items()))
        generation_seconds = time.perf_counter() - started
        registry = {"seeds": set(), "boards": set()}
        for previous in (work / "match3_difficulty" / "dataset", work / "match3_pool" / "dataset"):
            fingerprints(previous, registry)
        for lane in ("easy", "normal", "hard", "mixed"):
            for chunk in sorted((work / "match3_pool" / "production" / lane / "chunks").iterdir()):
                fingerprints(chunk, registry)
        prior_candidate_count = len(registry["seeds"])
        all_rows, all_settings, all_certified = [], [], []
        for directory, info in generated:
            fields, rows, settings_fields, settings, certified = validate_recipe(directory, info, previous_settings)
            fingerprints(directory, registry)
            all_rows.extend(rows)
            all_settings.append(settings)
            all_certified.extend((directory, row) for row in certified)
        validation_started = time.perf_counter()
        verify_certificates(verifier, all_certified)
        replay_seconds = time.perf_counter() - validation_started
        staged_dataset = staging / "dataset"
        for category in ("boards", "certificates"):
            (staged_dataset / category).mkdir(parents=True, exist_ok=True)
            for directory, _ in generated:
                for file in (directory / category).iterdir():
                    file.replace(staged_dataset / category / file.name)
        write_csv(staged_dataset / "candidates.csv", fields, all_rows)
        write_csv(staged_dataset / "settings.csv", settings_fields, all_settings)
        staged_dataset.replace(final_dataset)

    data = {**protocol, "candidates": len(all_rows), "certified": len(all_certified),
            "prior_candidates_checked_for_overlap": prior_candidate_count,
            "verdicts": dict(Counter(row["verdict"] for row in all_rows)),
            "rows_per_recipe": [info for _, info in generated], "independent_cli_replays_passed": len(all_certified),
            "parallel_generation_wall_seconds": generation_seconds, "replay_wall_seconds": replay_seconds,
            "total_wall_seconds": time.perf_counter() - started,
            "candidate_csv_sha256": sha256(final_dataset / "candidates.csv"),
            "settings_csv_sha256": sha256(final_dataset / "settings.csv")}
    (output / "generation-report.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {len(all_certified)}/{len(all_rows)} candidates; no overlap with {prior_candidate_count} earlier candidates.")


if __name__ == "__main__":
    main()
