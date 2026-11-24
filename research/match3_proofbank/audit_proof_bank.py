#!/usr/bin/env python3
"""Seal and independently replay a frozen Proof-Bank expansion delta."""

import argparse
import csv
import hashlib
import io
import json
import struct
import subprocess
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


SOLVER_COMMIT = "71aac8aab2e3fae2fa798e075f0f7d52b1220754"
SCHEMA = "match3-proof-bank-delta-v1"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def digest_record(path):
    return {"bytes": path.stat().st_size, "sha256": sha256(path)}


def canonical_hash(value):
    encoded = json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def csv_file(path):
    with path.open(newline="", encoding="utf-8-sig") as source:
        return list(csv.DictReader(source))


def by_id(rows):
    indexed = {}
    for row in rows:
        if row["id"] in indexed:
            raise ValueError(f"Duplicate board ID: {row['id']}")
        indexed[row["id"]] = row
    return indexed


def baseline_manifest(dataset, ids):
    input_csv = dataset / "candidates.csv"
    certified = by_id(row for row in csv_file(input_csv)
                      if row["verdict"] == "WIN_CERTIFIED" and row["cert_verified"] == "1")
    if not set(ids) <= set(certified):
        raise ValueError("An archived board does not come from the verified input dataset")
    return {
        "candidates.csv": digest_record(input_csv),
        "boards": {
            level: {
                "board": digest_record(dataset / "boards" / f"{level}.txt"),
                "certificate": digest_record(dataset / "certificates" / f"{level}.m3cert"),
                "goal": int(certified[level]["goal"]),
                "moves": int(certified[level]["moves"]),
            }
            for level in sorted(ids)
        },
    }


def archive_files(entries):
    files = list(entries.iterdir())
    if not files or any(path.suffix != ".m3bank" or not path.is_file() or path.is_symlink() for path in files):
        raise ValueError("Expected only board-level .m3bank archives")
    return {path.stem: path for path in files}


def reconcile(rows, expected_path, count_field):
    expected = by_id(csv_file(expected_path))
    seen = by_id(rows)
    if set(expected) != set(seen):
        raise ValueError("Expansion counts and archived board IDs differ")
    for level, row in seen.items():
        if int(row["entries"]) != int(expected[level][count_field]):
            raise ValueError(f"Archived promotion count disagrees with ledger for {level}")


def logged_fingerprint(encoded_board, goal, moves):
    # Log linkage only. The C++ audit and the manifest separately validate
    # literal state bytes and use SHA-256 for archive content identity.
    key = bytes.fromhex(encoded_board) + struct.pack("<ii", int(goal), int(moves))
    mask = (1 << 64) - 1
    value = 14695981039346656037
    for byte in key:
        value = ((value ^ byte) * 1099511628211) & mask
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
    return f"{value ^ (value >> 31):016x}"


def archived_queries(archives):
    records = {}
    for path in archives.values():
        with path.open("rb") as source:
            for line in iter(source.readline, b""):
                parts = line.decode("ascii").split()
                wave_count = int(parts[13])
                goal, moves, board, proof_size = parts[14 + wave_count:]
                key = (parts[1], int(parts[2]))
                value = (logged_fingerprint(parts[10], parts[8], parts[9]), parts[11], parts[12],
                         logged_fingerprint(board, goal, moves), goal, moves)
                if key in records:
                    raise ValueError(f"Duplicate framed proof ordinal {key}")
                records[key] = value
                source.seek(int(proof_size), 1)
    return records


def query_identity(row):
    return row["id"], row["successor_fp"]


def query_metadata(row):
    return tuple(row[field] for field in ("state_fp", "swap_a", "swap_b", "successor_fp", "successor_goal", "successor_moves"))


def verify_query_links(archives, query_path, base_query_path):
    expected = {}
    dispositions = Counter()
    seen = set()
    for row in csv_file(query_path):
        query = query_identity(row)
        if query in seen:
            raise ValueError(f"Repeated successor query {query}")
        seen.add(query)
        dispositions[row["disposition"]] += 1
        if row["disposition"] == "verified_safe":
            key = (row["id"], int(row["proof_number"]))
            if row["base_indexed_unknown"] != "0" or key[1] <= 0 or key in expected:
                raise ValueError("Invalid verification promotion or overlapping proof number")
            expected[key] = query_metadata(row)
        elif row["proof_number"] != "0":
            raise ValueError("Non-verified continuation claims a proof number")
    if archived_queries(archives) != expected:
        raise ValueError("Proof frames do not agree with exact action, successor and ordinal in promoted queries")

    base_unseen = None
    if base_query_path:
        base = {query_identity(row): row for row in csv_file(base_query_path)}
        for (level, _), metadata in expected.items():
            row = base[level, metadata[3]]
            if row["disposition"] != "base_control" or row["base_indexed_unknown"] != "0" or query_metadata(row) != metadata:
                raise ValueError("A promoted proof was not the matched unseen successor of the base control")
        base_unseen = len(expected)
    return {"archive_query_links_verified": len(expected), "matched_base_unseen_verified": base_unseen,
            "query_dispositions": dict(dispositions)}


def verify_cli_samples(cli, samples, rows, workers):
    audited = [row for row in rows if int(row["entries"]) > 0]

    def command(row):
        if (row["sample_colors"], row["sample_cap"], row["sample_depth"]) != ("5", "12", "9"):
            raise ValueError("CLI sample corpus expects the project rule domain (five colors, cap 12, depth 9)")
        base = samples / f"{row['id']}-{row['sample_ordinal']}"
        return [cli, "verify", base.with_suffix(".txt"), row["sample_goal"], row["sample_moves"], base.with_suffix(".m3cert")]

    def verify(row):
        result = subprocess.run(command(row), check=True, capture_output=True, text=True)
        if result.stdout.strip() != "PASS":
            raise ValueError(f"CLI did not certify archived sample for {row['id']}")

    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(verify, audited))

    negative = None
    if audited:
        wrong_goal = command(audited[0])
        wrong_goal[3] = str(int(wrong_goal[3]) + 1)
        result = subprocess.run(wrong_goal, check=False, capture_output=True, text=True)
        if result.returncode == 0 or "PASS" in result.stdout:
            raise ValueError("CLI accepted the certificate against a different target")
        negative = audited[0]["id"]
    return {"samples_verified": len(audited), "selection": "largest continuation certificate in each nonempty board archive",
            "wrong_goal_rejected_on": negative}


def seal(args):
    checkout = subprocess.check_output(["git", "-C", args.solver_checkout, "rev-parse", "HEAD"], text=True).strip()
    if checkout != SOLVER_COMMIT:
        raise ValueError("Solver revision does not match the audited release")
    archives = archive_files(args.entries)
    base = baseline_manifest(args.dataset, archives)
    if args.output.exists():
        raise FileExistsError(args.output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="proof-bank-audit-") as scratch:
        samples = Path(scratch) / "samples"
        result = subprocess.run([args.verifier, args.entries, samples], check=True, capture_output=True, text=True)
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        if set(by_id(rows)) != set(archives):
            raise ValueError("Independent verifier omitted a board archive")
        if args.expansion_csv:
            reconcile(rows, args.expansion_csv, args.expansion_count_field)
        cli = verify_cli_samples(args.solver_cli, samples, rows, args.workers)

    inputs = {}
    for path in args.source:
        if path.name in inputs:
            raise ValueError(f"Duplicate version-bound source name: {path.name}")
        inputs[path.name] = digest_record(path)
    totals = {"boards": len(rows), "expanded_continuations": sum(int(row["entries"]) for row in rows),
              "certificate_payload_bytes": sum(int(row["payload_bytes"]) for row in rows)}
    intended = {
        "schema": SCHEMA,
        "format": "M3PBK1 length-framed metadata and M3CERT 1 certificate, one stream per original board",
        "scope": "verified additions to the pinned base catalog, with exact parent, legal action, full refill trace and successor",
        "solver_commit": checkout,
        "rules": {"refill_cap": 12, "cascade_limit": 9, "colors": 5},
        "source_and_configuration_files": inputs,
        "baseline_inputs": base,
        "delta_files": {path.name: digest_record(path) for path in sorted(archives.values())},
        "totals": totals,
    }
    if args.expansion_csv:
        intended["promotion_ledger"] = {"file": args.expansion_csv.name, **digest_record(args.expansion_csv)}
    links = {}
    if args.query_csv:
        links = verify_query_links(archives, args.query_csv, args.base_query_csv)
        intended["targeted_query_ledger"] = {"file": args.query_csv.name, **digest_record(args.query_csv)}
        if args.base_query_csv:
            intended["base_query_ledger"] = {"file": args.base_query_csv.name, **digest_record(args.base_query_csv)}

    args.output.mkdir()
    (args.output / "verified-entries.csv").write_text(result.stdout, encoding="utf-8")
    dump(args.output / "independent-replay.json", {**totals, "independently_replayed_full_edges": totals["expanded_continuations"],
                                                  "independently_checked_full_child_proofs": totals["expanded_continuations"],
                                                  **cli, **links})
    digest = canonical_hash(intended)
    dump(args.output / "proof-bank-manifest.json", {**intended, "artifact_sha256": digest})
    print(json.dumps({**totals, **cli, **links, "artifact_sha256": digest}, ensure_ascii=False))


def verify_manifest(args):
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    expected_hash = manifest.pop("artifact_sha256")
    if manifest["schema"] != SCHEMA or manifest["solver_commit"] != SOLVER_COMMIT or canonical_hash(manifest) != expected_hash:
        raise ValueError("Proof-Bank manifest digest mismatch")
    archives = archive_files(args.entries)
    if baseline_manifest(args.dataset, archives) != manifest["baseline_inputs"]:
        raise ValueError("Original certificate or board bytes differ from the sealed manifest")
    actual = {path.name: digest_record(path) for path in sorted(archives.values())}
    if actual != manifest["delta_files"]:
        raise ValueError("Expanded certificate bytes differ from the sealed manifest")
    print(f"Verified {len(archives)} proof archives at SHA-256 {expected_hash}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("seal")
    create.add_argument("entries", type=Path)
    create.add_argument("dataset", type=Path)
    create.add_argument("output", type=Path)
    create.add_argument("--verifier", type=Path, required=True)
    create.add_argument("--solver-cli", type=Path, required=True)
    create.add_argument("--solver-checkout", type=Path, required=True)
    create.add_argument("--source", action="append", type=Path, default=[])
    create.add_argument("--workers", type=int, default=8)
    create.add_argument("--expansion-csv", type=Path)
    create.add_argument("--expansion-count-field", default="promoted")
    create.add_argument("--query-csv", type=Path)
    create.add_argument("--base-query-csv", type=Path)
    create.set_defaults(run=seal)
    check = commands.add_parser("verify")
    check.add_argument("manifest", type=Path)
    check.add_argument("entries", type=Path)
    check.add_argument("dataset", type=Path)
    check.set_defaults(run=verify_manifest)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
