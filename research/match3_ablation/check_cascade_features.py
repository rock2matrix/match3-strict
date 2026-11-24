"""Cross-check the new score extraction against the earlier experiment tables."""

import argparse
import csv
from pathlib import Path


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    keyed = {(row["id"], row["agent"]): row for row in rows}
    if len(keyed) != len(rows):
        raise ValueError(f"Duplicate level/agent in {path}")
    return keyed


def compare(cascade, previous, previous_means):
    assert cascade.keys() == previous.keys() == previous_means.keys()
    max_p_error = 0
    max_clear_error = 0
    for key, row in cascade.items():
        early = float(row["exact_early"])
        challenge = float(row["exact_challenge"])
        joint = float(row["exact_early_and_challenge"])
        assert max(0.0, early + challenge - 1) - 1e-12 <= joint <= min(early, challenge) + 1e-12, key
        for field in ("exact_early", "exact_challenge", "exact_chain"):
            max_p_error = max(max_p_error, abs(float(previous[key][field]) - float(row[field])))

        total = float(row["exact_mean_clears"])
        expected_total = float(previous_means[key]["exact_mean_surplus"]) + int(row["goal"])
        max_clear_error = max(max_clear_error, abs(total - expected_total))

        ge3, ge6, ge9 = (float(row[f"exact_extra_ge{threshold}"]) for threshold in (3, 6, 9))
        capped6, capped9 = (float(row[f"exact_extra_tiles_capped{threshold}"]) for threshold in (6, 9))
        extra = float(row["exact_mean_extra_tiles"])
        productive = float(row["exact_mean_productive_extra_tiles"])
        assert 1 + 1e-12 >= ge3 >= ge6 - 1e-12 >= ge9 - 2e-12 >= -2e-12, key
        assert ge9 - 1e-12 <= capped9 <= capped6 + 1e-12 and ge6 - 1e-12 <= capped6 <= ge3 + 1e-12, key
        assert abs(ge3 - float(row["exact_chain"])) < 1e-12, key
        assert extra + 1e-12 >= 3 * (ge3 + ge6 + ge9), key
        assert -1e-12 <= productive <= min(extra, int(row["goal"])) + 1e-12 <= total + 1e-12, key

    assert max_p_error < 1e-11 and max_clear_error < 1e-10
    print(f"{len(cascade)} agent rows: max historical probability delta {max_p_error:.3g}; "
          f"max historical expected-clear delta {max_clear_error:.3g}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cascade", type=Path)
    parser.add_argument("previous_policies", type=Path)
    parser.add_argument("previous_exact_features", type=Path)
    args = parser.parse_args()
    compare(read_rows(args.cascade), read_rows(args.previous_policies), read_rows(args.previous_exact_features))


if __name__ == "__main__":
    main()
