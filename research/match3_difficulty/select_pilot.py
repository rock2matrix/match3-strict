"""Select verified boards for an exploratory human pilot from agent results."""

import argparse
import csv
from collections import Counter, defaultdict
from decimal import Decimal
from pathlib import Path


AGENTS = ("Random", "Greedy", "Objective", "Lookahead")
ENDPOINTS = ("challenge", "early", "chain")
ANCHOR_PERCENTILES = (0, 20, 40, 60, 80, 100)
GROUP_SIZE = 25
MEANINGFUL_DISAGREEMENT = Decimal("0.15")


def read_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def unique_index(rows, key):
    result = {key(row): row for row in rows}
    if len(result) != len(rows):
        raise ValueError("Input contains duplicate row keys.")
    return result


def random_band(rank):
    if rank <= 8:
        return "low"
    if rank <= 17:
        return "middle"
    return "high"


def score_level(candidate, level, policies):
    if (candidate["goal"], candidate["moves"]) != (level["goal"], level["moves"]):
        raise ValueError(f"Goal or move limit mismatch for {candidate['id']}.")
    rates = {
        f"{agent.lower()}_exact_{endpoint}": policies[agent][f"exact_{endpoint}"]
        for agent in AGENTS
        for endpoint in ENDPOINTS
    }
    if any(not Decimal(0) <= Decimal(value) <= Decimal(1) for value in rates.values()):
        raise ValueError(f"Out-of-range probability for {candidate['id']}.")
    random_score = Decimal(rates["random_exact_challenge"])
    return {
        **{field: candidate[field] for field in ("id", "seed", "stratum", "goal", "moves")},
        "cert_verified": "1",
        "sampling_purpose": "exploratory_human_pilot",
        "d1_pressure": level["d1_pressure"],
        **rates,
        "greedy_vs_random_challenge_gap": Decimal(rates["greedy_exact_challenge"]) - random_score,
        "lookahead_vs_random_challenge_gap": Decimal(rates["lookahead_exact_challenge"]) - random_score,
    }


def select_group(group):
    if len(group) != GROUP_SIZE:
        raise ValueError(f"Expected {GROUP_SIZE} certified levels per stratum, got {len(group)}.")
    ranked = sorted(group, key=lambda row: (Decimal(row["random_exact_challenge"]), row["id"]))
    for rank, row in enumerate(ranked, start=1):
        row["random_challenge_rank_within_stratum"] = rank
        row["random_challenge_band_within_stratum"] = random_band(rank)

    selected = {}
    for percentile in ANCHOR_PERCENTILES:
        index = ((len(ranked) - 1) * percentile + 50) // 100
        row = ranked[index]
        selected[row["id"]] = {
            **row,
            "selection_type": "random_rank_anchor",
            "selection_reason": f"Random 主挑战率组内第 {index + 1}/{len(ranked)} 名",
        }

    remaining = [row for row in ranked if row["id"] not in selected]
    pairs = [
        (greedy, lookahead)
        for greedy in remaining
        for lookahead in remaining
        if greedy["id"] != lookahead["id"]
    ]

    def pair_key(pair):
        greedy, lookahead = pair
        greedy_gap = greedy["greedy_vs_random_challenge_gap"]
        lookahead_gap = lookahead["lookahead_vs_random_challenge_gap"]
        # Prefer an increase and a decrease if both differ by at least 15 points.
        opposite_signs = (greedy_gap * lookahead_gap < 0 and
                          min(abs(greedy_gap), abs(lookahead_gap)) >= MEANINGFUL_DISAGREEMENT)
        return (
            not opposite_signs,
            -(abs(greedy_gap) + abs(lookahead_gap)),
            -min(abs(greedy_gap), abs(lookahead_gap)),
            greedy["id"],
            lookahead["id"],
        )

    for agent, row in zip(("Greedy", "Lookahead"), min(pairs, key=pair_key)):
        key = agent.lower()
        gap = 100 * row[f"{key}_vs_random_challenge_gap"]
        selected[row["id"]] = {
            **row,
            "selection_type": f"{key}_disagreement",
            "selection_reason": f"{agent} 主挑战率相对 Random {gap:+.2f} 个百分点",
        }
    return sorted(selected.values(), key=lambda row: row["random_challenge_rank_within_stratum"])


def main():
    base = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, default=base / "dataset")
    parser.add_argument("--results", type=Path, default=base / "results")
    args = parser.parse_args()

    candidates = unique_index(read_rows(args.dataset / "candidates.csv"), lambda row: row["id"])
    levels = unique_index(read_rows(args.results / "levels.csv"), lambda row: row["id"])
    policies = unique_index(read_rows(args.results / "policies.csv"), lambda row: (row["id"], row["agent"]))
    verified = {
        board_id: row for board_id, row in candidates.items()
        if row["verdict"] == "WIN_CERTIFIED" and row["cert_verified"] == "1"
    }
    if set(verified) != set(levels) or set(policies) != {(board_id, agent) for board_id in verified for agent in AGENTS}:
        raise ValueError("Verified candidates, simulated levels and policy rows must match.")

    groups = defaultdict(list)
    for board_id, candidate in verified.items():
        board_policies = {agent: policies[board_id, agent] for agent in AGENTS}
        groups[candidate["stratum"]].append(score_level(candidate, levels[board_id], board_policies))
    if len(groups) != 4:
        raise ValueError("Expected four strata in the certified pool.")

    shortlist = []
    for stratum in sorted(groups):
        selected = select_group(groups[stratum])
        shortlist.extend(selected)
        bands = Counter(row["random_challenge_band_within_stratum"] for row in selected)
        print(f"{stratum}: {len(selected)} levels, Random bands {dict(bands)}")

    output = args.results / "human_pilot_shortlist.csv"
    temporary = output.with_suffix(".csv.tmp")
    with temporary.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "id", "seed", "stratum", "goal", "moves", "cert_verified", "sampling_purpose",
            "selection_type", "selection_reason", "random_challenge_rank_within_stratum",
            "random_challenge_band_within_stratum", "d1_pressure",
            *(f"{agent.lower()}_exact_{endpoint}" for agent in AGENTS for endpoint in ENDPOINTS),
            "greedy_vs_random_challenge_gap", "lookahead_vs_random_challenge_gap",
        ])
        writer.writeheader()
        writer.writerows(shortlist)
    temporary.replace(output)
    print(f"Wrote {len(shortlist)} unique levels to {output}")


if __name__ == "__main__":
    main()
