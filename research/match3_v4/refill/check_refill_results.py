import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
CONTROLLERS = ("Witness", "ConditionalRandom", "MinIntervention")
METRICS = ("early", "challenge", "chain", "mean_productive_extra_tiles")


def rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def keyed(table, fields):
    by_key = {tuple(row[field] for field in fields): row for row in table}
    assert len(by_key) == len(table), f"duplicate rows at {fields}"
    return by_key


def check_summary(root, archived_path):
    summary = keyed(rows(root / "summary.csv"), ("id", "agent", "controller"))
    exact = keyed(rows(root / "witness-exact.csv"), ("id", "agent"))
    oracle = keyed(rows(root / "oracle-board.csv"), ("id",))
    archived = keyed(rows(archived_path), ("id", "agent"))
    ids = {identifier for identifier, _ in exact}
    assert set(summary) == {(identifier, agent, ctl) for identifier in ids for agent in AGENTS for ctl in CONTROLLERS}
    assert set(exact) == {(identifier, agent) for identifier in ids for agent in AGENTS}
    assert set(oracle) == {(identifier,) for identifier in ids}

    max_archived_delta = {metric: 0.0 for metric in METRICS}
    max_paired_delta = {agent: {metric: 0.0 for metric in METRICS} for agent in AGENTS}
    paired_differences = Counter()
    for key, row in exact.items():
        _, agent = key
        for metric in METRICS:
            column = "exact_" + metric
            max_archived_delta[metric] = max(max_archived_delta[metric], abs(float(row[column]) - float(archived[key][column])))
            difference = abs(float(row["paired_" + column]) - float(row[column]))
            max_paired_delta[agent][metric] = max(max_paired_delta[agent][metric], difference)
            if difference > 1e-10:
                paired_differences[(agent, metric)] += 1
    assert max(max_archived_delta.values()) < 1e-10, max_archived_delta
    assert max(max_paired_delta["Random"].values()) < 1e-10
    assert max(max_paired_delta["Greedy"].values()) < 1e-10

    observed = defaultdict(Counter)
    for row in rows(root / "episodes.csv"):
        identifier, agent, ctl, episode = (row[k] for k in ("id", "agent", "controller", "run"))
        record = observed[identifier, agent, ctl]
        totals = summary[identifier, agent, ctl]
        goal, moves = int(totals["goal"]), int(totals["moves"])
        turns, clears, left, chain, productive, extra, waves, proposals, fallbacks = (
            int(row[k]) for k in ("turns", "clears", "left", "chain", "productive_extra", "extra_waves", "max_waves", "proposals", "fallbacks")
        )
        assert int(episode) == record["episodes"], (identifier, agent, ctl, episode)
        assert 0 < turns <= moves and turns + left == moves and clears >= goal and 0 <= productive <= goal
        assert chain == (extra > 0) == (waves > 1)
        assert 0 <= fallbacks <= turns
        if ctl == "Witness":
            assert proposals == fallbacks == 0
        elif ctl == "MinIntervention":
            assert proposals == turns
        else:
            assert turns <= proposals <= 6 * turns
        record.update({"episodes": 1, "wins": 1, "early": int(left > 0), "challenge": int(clears >= goal + 3),
                       "chain": chain, "productive_extra": productive, "clears": clears, "total_turns": turns,
                       "proposal_attempts": proposals, "witness_fallbacks": fallbacks, "extra_waves": extra})
        record["max_waves"] = max(record["max_waves"], waves)

    waves = defaultdict(Counter)
    for row in rows(root / "wave-sizes.csv"):
        key = tuple(row[field] for field in ("id", "agent", "controller"))
        wave, tiles, count = (int(row[field]) for field in ("wave", "tiles", "count"))
        assert 1 <= wave <= 9 and 3 <= tiles <= 12 and count > 0
        group = waves[key]
        group["clears"] += tiles * count
        group["extra_waves"] += count if wave > 1 else 0
        group["total_turns"] += count if wave == 1 else 0
        group["max_waves"] = max(group["max_waves"], wave)

    assert set(summary) == set(observed) == set(waves)
    by_board = defaultdict(Counter)
    by_controller = defaultdict(Counter)
    for key, row in summary.items():
        identifier, agent, ctl = key
        numeric = {k: int(v) for k, v in row.items() if k not in ("id", "seed", "recipe", "agent", "controller")}
        for counts in (observed[key], waves[key]):
            assert all(numeric[field] == value for field, value in counts.items()), (key, counts, numeric)
        assert numeric["episodes"] == numeric["wins"] == 64
        assert numeric["proposal_attempts"] == numeric["proposal_safe"] + numeric["proposal_unsafe"] + numeric["proposal_unknown"]
        assert numeric["total_turns"] == numeric["executed_witness"] + numeric["executed_proposal"]
        assert numeric["proposal_safe"] == numeric["executed_proposal"]
        if ctl == "MinIntervention":
            assert numeric["proposal_safe"] == numeric["first_accepted"]
        by_board[identifier].update(numeric)
        by_controller[ctl].update(numeric)

    for (identifier,), row in oracle.items():
        board = by_board[identifier]
        assert int(row["proposals"]) == board["proposal_attempts"]
        assert int(row["natural_edges_replayed"]) == board["proposal_safe"]
        assert int(row["solver_calls"]) == board["solver_calls"]
        assert int(row["cache_hits"]) == board["cache_hits"]
        assert int(row["same_witness"]) == board["proposal_same_witness"]
        assert int(row["unsafe_refill_cap"]) == board["proposal_cap"]
        assert int(row["unknown_depth"]) == board["proposal_depth"]
        assert int(row["unknown_work_cap"]) == 0

    paired_errors = {agent: {metric: [] for metric in METRICS} for agent in AGENTS}
    for (identifier, agent), row in exact.items():
        result = observed[identifier, agent, "Witness"]
        for metric in METRICS:
            observed_name = "productive_extra" if metric == "mean_productive_extra_tiles" else metric
            difference = result[observed_name] / result["episodes"] - float(row["paired_exact_" + metric])
            paired_errors[agent][metric].append(abs(difference))

    return {"boards": len(ids), "archived_max_abs_difference": max_archived_delta,
            "paired_exact_vs_archive_max_abs_difference": max_paired_delta,
            "paired_exact_cells_differing_from_archive": {"/".join(k): v for k, v in paired_differences.items()},
            "witness_mean_abs_sampling_error": {agent: {metric: sum(values) / len(values) for metric, values in metrics.items()}
                                                for agent, metrics in paired_errors.items()},
            "controller_counts": {key: dict(value) for key, value in by_controller.items()}}


def check_pilot(pilot, full):
    def records(file, prefix=None):
        out = rows(file)
        for row in out:
            row.pop("elapsed_ms", None)
        return [row for row in out if prefix is None or row["id"] in prefix]

    ids = {row["id"] for row in records(pilot / "summary.csv")}
    assert len(ids) == 32
    for table in ("summary.csv", "episodes.csv", "witness-exact.csv", "wave-sizes.csv", "oracle-board.csv"):
        assert records(pilot / table) == records(full / table, ids), table


def main():
    parser = argparse.ArgumentParser(description="Verify archived witness parity and every refill counter")
    parser.add_argument("results", type=Path)
    parser.add_argument("archived_witness", type=Path)
    parser.add_argument("--pilot", type=Path)
    args = parser.parse_args()
    validation = check_summary(args.results, args.archived_witness)
    if args.pilot:
        check_pilot(args.pilot, args.results)
        validation["pilot_matches_full"] = True
    print(json.dumps(validation, indent=2))


if __name__ == "__main__":
    main()
