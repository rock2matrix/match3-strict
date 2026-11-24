import argparse
import csv
import itertools
import json
import math
from pathlib import Path


def rows(path):
    with path.open(encoding="utf-8", newline="") as stream:
        yield from csv.DictReader(stream)


def group_key(row):
    return tuple(row[key] for key in ("controller", "cache", "scenario", "repeat"))


def validate(root):
    fixtures = list(rows(root / "fixtures.csv"))
    summary = {group_key(row): row for row in rows(root / "load-summary.csv")}
    assert [int(row["fixture"]) for row in fixtures] == list(range(len(fixtures)))
    assert {row["recipe"] for row in fixtures} == {"g6-m3", "g10-m4", "g8-m2", "g10-m3"}
    assert {int(row["turns_remaining"]) for row in fixtures} >= {1, 2, 3, 4}
    assert all(0 <= int(row["acr_planned"]) <= 6 for row in fixtures)
    counters = {"groups": 0, "offered": 0, "admitted": 0, "rejected": 0, "verified_natural": 0,
                "completion_checks_passed": 0, "cpu_over_2ms": 0, "deadline_capped": 0}
    seen = set()
    for key, batch in itertools.groupby(rows(root / "load-requests.csv"), group_key):
        assert key not in seen
        seen.add(key)
        target = summary[key]
        totals = {column: 0 for column in (
            "offered", "admitted", "rejected", "attempts_planned", "attempts_used", "witness_fallbacks",
            "cache_hits", "natural_replays", "verified_natural", "deadline_capped", "work_capped", "cpu_over_2ms",
            "unverified", "witness_uses",
        )}
        fields = {column: [] for column in ("cpu", "service_wall", "queue", "due_to_finish", "dispatch_delay")}
        maximum = {column: 0 for column in ("attempts_used", "candidate_checks", "refill_nodes", "state_expansions")}
        for index, row in enumerate(batch):
            assert None not in row and all(value is not None for value in row.values())
            assert int(row["index"]) == index and int(row["fixture"]) == index % len(fixtures)
            assert int(row["worker"]) == index % int(target["workers"])
            totals["offered"] += 1
            if row["admitted"] == "0":
                assert all(row[field] == "" for field in ("start_ns", "cpu_ns", "attempts_used", "verified"))
                totals["rejected"] += 1
                continue
            totals["admitted"] += 1
            totals["unverified"] += 1 - int(row["verified"])
            due, enqueued, start, finish, cpu = (
                int(row[field]) for field in ("due_ns", "enqueued_ns", "start_ns", "finish_ns", "cpu_ns"))
            assert 0 <= due <= enqueued <= start <= finish
            assert 0 <= cpu <= finish - start
            fields["cpu"].append(cpu)
            fields["service_wall"].append(finish - start)
            fields["queue"].append(start - enqueued)
            fields["due_to_finish"].append(finish - due)
            fields["dispatch_delay"].append(enqueued - due)
            assert row["attempts_used"] == row["candidate_checks"]
            planned = (int(fixtures[index % len(fixtures)]["acr_planned"]) if key[0] == "ACR" else
                       {"Witness": 0, "Fixed1": 1, "Fixed6": 6}[key[0]])
            assert int(row["attempts_planned"]) == planned
            assert 0 <= int(row["attempts_used"]) <= planned <= 6
            assert int(row["verified_natural"]) <= int(row["natural_replays"]) <= 1
            assert int(row["used_witness"]) + int(row["verified_natural"]) == 1
            assert not (int(row["witness_fallback"]) and int(row["verified_natural"]))
            if key[1] == "ColdNoMemo":
                assert row["cache_hits"] == "0"
            for column in maximum:
                maximum[column] = max(maximum[column], int(row[column]))
            for column in ("attempts_planned", "attempts_used", "cache_hits", "natural_replays",
                           "verified_natural", "deadline_capped", "work_capped"):
                totals[column] += int(row[column])
            totals["witness_fallbacks"] += int(row["witness_fallback"])
            totals["witness_uses"] += int(row["used_witness"])
            totals["cpu_over_2ms"] += cpu > 2_000_000

        for column, value in totals.items():
            assert int(target[column]) == value, (key, column, target[column], value)
        for column, value in maximum.items():
            assert int(target[f"max_{column}"]) == value
        assert maximum["candidate_checks"] <= 512 and maximum["refill_nodes"] <= 8192 and maximum["state_expansions"] <= 128
        assert totals["offered"] == totals["admitted"] + totals["rejected"] and totals["unverified"] == 0
        for column, raw in fields.items():
            ordered = sorted(raw)
            for pct in (50, 95, 99):
                expected = ordered[math.ceil(len(ordered) * pct / 100) - 1] / 1000
                assert math.isclose(float(target[f"{column}_p{pct}_us"]), expected, rel_tol=5e-8, abs_tol=1e-8)
        assert math.isclose(float(target["decision_cpu_s"]), sum(fields["cpu"]) / 1e9, rel_tol=5e-8, abs_tol=1e-8)
        counters["groups"] += 1
        for column in ("offered", "admitted", "rejected", "verified_natural", "cpu_over_2ms", "deadline_capped"):
            counters[column] += totals[column]
        counters["completion_checks_passed"] += totals["admitted"]
    assert seen == set(summary)
    return {"fixtures": len(fixtures), "boards": len({row["id"] for row in fixtures}), **counters}


def main():
    parser = argparse.ArgumentParser(description="Reconcile ACR load arrivals, work, timing and percentiles")
    parser.add_argument("load_directory", type=Path)
    args = parser.parse_args()
    result = validate(args.load_directory)
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    (args.load_directory / "validation.json").write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
