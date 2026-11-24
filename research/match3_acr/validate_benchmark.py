import argparse
import csv
import itertools
import json
from collections import Counter, defaultdict
from pathlib import Path


def rows(path):
    with Path(path).open(newline="", encoding="utf-8") as source:
        yield from csv.DictReader(source)


def fail(condition, reason):
    if not condition:
        raise ValueError(reason)


def check_benchmark(directory):
    summaries = {tuple(row[k] for k in ("id", "agent", "controller")): row for row in rows(directory / "summary.csv")}
    totals = defaultdict(Counter)
    episode_count = Counter()
    episode_hist = defaultdict(Counter)
    episode_turns = defaultdict(Counter)
    for row in rows(directory / "episodes.csv"):
        key = tuple(row[k] for k in ("id", "agent", "controller"))
        expected = summaries[key]
        goal, moves = int(expected["goal"]), int(expected["moves"])
        fail(int(row["run"]) == episode_count[key], f"episode order or duplicate at {key}")
        episode_count[key] += 1
        check = tuple((*key, row["run"]))
        clear, turns, left = int(row["clears"]), int(row["turns"]), int(row["left"])
        fail(clear >= goal and left == moves - turns and 0 <= int(row["productive_extra"]) <= goal,
             f"episode target, move, or productive count mismatch at {check}")
        episode_hist[key].update({
            "wins": 1, "early": int(left > 0), "challenge": int(clear >= goal + 3), "chain": int(row["chain"]),
            "productive_extra": int(row["productive_extra"]), "clears": clear, "total_turns": turns,
            "proposal_attempts": int(row["proposals"]), "witness_fallbacks": int(row["fallbacks"]),
        })
    clocks = defaultdict(lambda: ([], []))
    plan_counts = defaultdict(Counter)
    max_caps = defaultdict(Counter)
    for row in rows(directory / "turns.csv"):
        key = tuple(row[k] for k in ("id", "agent", "controller"))
        check = (*key, row["run"], row["turn"])
        fields = {k: int(row[k]) for k in ("candidate_checks", "refill_nodes", "states", "used_witness", "fallback",
                   "proposal_safe", "proposal_unsafe", "proposal_unknown", "k", "cache_hits", "timeout", "capped",
                   "same_witness", "planned_witness", "waves")}
        fail(0 <= fields["candidate_checks"] <= min(512, fields["k"]) and fields["refill_nodes"] <= 8192 and
             fields["states"] <= 128, f"deterministic online cap at {check}")
        fail(sum(fields[k] for k in ("proposal_safe", "proposal_unsafe", "proposal_unknown")) == fields["candidate_checks"],
             f"proposal category mismatch at {check}")
        fail(bool(fields["used_witness"]) or fields["proposal_safe"] == 1, f"unverified execution at {check}")
        fail(not fields["fallback"] or fields["used_witness"], f"fallback executed non-witness at {check}")
        fail(not fields["planned_witness"] or (key[2] == "Adaptive" and fields["k"] == 0 and fields["fallback"]),
             f"planned Witness not counted at {check}")
        fail(fields["k"] == {"Witness": 0, "Fixed1": 1, "Fixed3": 3, "Fixed6": 6}.get(key[2], fields["k"]),
             f"fixed budget changed at {check}")
        wave, original = row["first_wave"], row["first_wave_witness"]
        color_count = 5
        expected = Counter({str(c): len(wave) // color_count + (c < len(wave) % color_count) for c in range(color_count)})
        fail(3 <= len(wave) <= 12 and Counter(wave) == Counter(original) == +expected and 1 <= fields["waves"] <= 9,
             f"natural quota mismatch at {check}")
        merged = {"total_turns": 1, "executed_witness": fields["used_witness"], "executed_proposal": 1-fields["used_witness"],
                  "witness_fallbacks": fields["fallback"], "proposal_attempts": fields["candidate_checks"],
                  "proposal_same_witness": fields["same_witness"], "timeout_fallbacks": fields["timeout"],
                  "cap_fallbacks": fields["capped"], "planned_witness_turns": fields["planned_witness"],
                  "extra_waves": fields["waves"] - 1}
        merged.update({key: fields[key] for key in ("candidate_checks", "refill_nodes", "states", "cache_hits",
                       "proposal_safe", "proposal_unsafe", "proposal_unknown")})
        episode_turns[key].update(merged)
        controller = key[2]
        totals[controller].update(merged)
        plan_counts[controller][fields["k"]] += 1
        clocks[controller][0].append(int(row["cpu_ns"]))
        clocks[controller][1].append(int(row["wall_ns"]))
        for limit in ("candidate_checks", "refill_nodes", "states"):
            max_caps[controller][limit] = max(max_caps[controller][limit], fields[limit])
    for key, expected in summaries.items():
        fail(episode_count[key] == int(expected["episodes"]) > 0, f"episode count mismatch at {key}")
        fail(all(int(expected[field]) == value for field, value in episode_hist[key].items()), f"episode aggregate mismatch at {key}")
        fail(all(int(expected[field]) == value for field, value in episode_turns[key].items()), f"turn aggregate mismatch at {key}")
    mass = Counter()
    offline = Counter()
    for row in rows(directory / "mass.csv"):
        z_low, z_point, z_upper = map(float, [row["z_lower"], row["z_point"], row["z_upper"]])
        fail(0 <= z_low <= z_point <= z_upper <= 1, f"invalid mass interval {row['id']}")
        if row["bucket"] == "all":
            fail(sum(int(row[key]) for key in ("safe", "unsafe", "unresolved")) == int(row["forecast_samples"]),
                 f"M1 counts mismatch {row['id']}")
            mass.update({key: int(row[key]) for key in ("slots", "safe", "unsafe", "unresolved", "forecast_samples")})
    for row in rows(directory / "offline-bank.csv"):
        offline.update({key: int(row[key]) for key in ("proposals", "solver_calls", "certificates_verified", "natural_edges_replayed",
                       "candidate_checks", "refill_nodes", "states", "cpu_ns", "wall_ns")})
    result = {"boards": len({key[0] for key in summaries}), "episodes": sum(episode_count.values()),
              "offline": offline, "mass_audit": mass, "arms": {}}
    for controller, total in totals.items():
        cpu, wall = (sorted(series) for series in clocks[controller])
        quantiles = lambda data: {str(q): data[min(len(data)-1, int(q/100*len(data)))]/1000 for q in (50, 95, 99)}
        result["arms"][controller] = {
            **total, "k_counts": plan_counts[controller], "max_counts_per_turn": max_caps[controller],
            "witness_reliance_rate": total["witness_fallbacks"] / total["total_turns"],
            "cpu_us": quantiles(cpu), "wall_us": quantiles(wall),
        }
    return result


def compare(dirs):
    details = {}
    fields = {"turns.csv": {"cpu_ns", "wall_ns", "cache_hits"}, "summary.csv": {"cache_hits"},
              "mass.csv": set(), "episodes.csv": set(), "witness-exact.csv": set(),
              "offline-bank.csv": {"cpu_ns", "wall_ns"}}
    for file, skip in fields.items():
        pairs = itertools.zip_longest(*(rows(directory / file) for directory in dirs))
        differences, pairs_count = 0, 0
        for left, right in pairs:
            if left is None or right is None or any(left[k] != right[k] for k in left if k not in skip):
                differences += 1
            pairs_count += 1
        details[file] = {"rows": pairs_count, "differences_excluding_timing_and_lookup_cache": differences}
    return details


def main():
    parser = argparse.ArgumentParser(description="Independently reconcile ACR trial rows, proof/attempt and quota counters")
    parser.add_argument("warm", type=Path)
    parser.add_argument("cold", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = {"warm": check_benchmark(args.warm), "cold": check_benchmark(args.cold), "comparison": compare((args.warm, args.cold))}
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
