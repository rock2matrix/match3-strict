#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || ( $# -eq 3 && $3 != --regenerate ) ]]; then
    printf 'Usage: bash reproduce.sh SOLVER_CHECKOUT NEW_RUN_DIRECTORY [--regenerate]\n' >&2
    exit 2
fi

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(dirname -- "$here")
solver=$(realpath -- "$1")
run=$(realpath -m -- "$2")
commit=71aac8aab2e3fae2fa798e075f0f7d52b1220754
if [[ $(git -C "$solver" rev-parse HEAD) != "$commit" ]] || ! git -C "$solver" diff --quiet HEAD --; then
    printf 'Use the clean solver checkout at %s.\n' "$commit" >&2
    exit 2
fi
if [[ -e $run ]]; then
    printf 'Run directory already exists: %s\n' "$run" >&2
    exit 2
fi
mkdir -p -- "$run"
cmake -S "$solver" -B "$run/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$run/build" --config Release --parallel 4
ctest --test-dir "$run/build" -C Release --output-on-failure
library="$run/build/libmatch3_strict.a"
python3 "$here/build_cascade.py" "$solver" "$library" "$run/build"

compare_features() {
    python3 - "$1" "$2" <<'PY'
import csv
import sys

def rows(path):
    with open(path, newline="", encoding="utf-8") as stream:
        return [{k: v for k, v in row.items() if k not in ("verify_us", "score_us")}
                for row in csv.DictReader(stream)]

assert rows(sys.argv[1]) == rows(sys.argv[2]), (sys.argv[1], sys.argv[2])
PY
}

cal="$work/match3_difficulty"
old="$work/match3_pool"
"$run/build/cascade-features" "$cal/dataset" "$run/calibration-cascade.csv"
"$run/build/cascade-features" "$old/dataset" "$run/holdout-cascade.csv"
compare_features "$run/calibration-cascade.csv" "$here/calibration-cascade.csv"
compare_features "$run/holdout-cascade.csv" "$here/holdout-cascade.csv"

# Use the archived inputs after checking the recalculated outcomes. Their
# machine timings are evidence and are part of the original selection hash.
python3 "$here/ablation.py" --protocol "$here/protocol.json" explore \
    --frozen-v1 "$old/frozen-standard.json" \
    --calibration-candidates "$cal/dataset/candidates.csv" --calibration-policies "$cal/results/policies.csv" \
    --calibration-features "$here/calibration-cascade.csv" --calibration-labels "$old/calibration-levels.csv" \
    --holdout-candidates "$old/dataset/candidates.csv" --holdout-policies "$old/holdout_sim/policies.csv" \
    --holdout-features "$here/holdout-cascade.csv" --holdout-labels "$old/validation/holdout-levels.csv" \
    --output "$run/results"
cmp -- "$here/results/selection-lock.json" "$run/results/selection-lock.json"

fresh="$here/fresh/dataset"
if [[ ${3:-} == --regenerate ]]; then
    "${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -I "$solver/include" \
        "$old/generate.cpp" "$library" -o "$run/build/generate"
    python3 "$here/fresh/generate_confirmation.py" --solver "$solver" --generator "$run/build/generate" \
        --verifier "$run/build/match3-solve" --previous "$work" --output "$run/fresh"
    fresh="$run/fresh/dataset"
    cmp "$fresh/settings.csv" "$here/fresh/dataset/settings.csv"
    diff <(cut -d, -f1-17 "$fresh/candidates.csv") <(cut -d, -f1-17 "$here/fresh/dataset/candidates.csv")
fi

"$run/build/cascade-features" "$fresh" "$run/confirmation-cascade.csv"
compare_features "$run/confirmation-cascade.csv" "$here/fresh/confirmation-cascade.csv"
python3 "$here/ablation.py" --protocol "$here/protocol.json" confirm \
    --selection-lock "$run/results/selection-lock.json" --calibration-candidates "$cal/dataset/candidates.csv" \
    --holdout-candidates "$old/dataset/candidates.csv" --confirmation-candidates "$fresh/candidates.csv" \
    --confirmation-features "$run/confirmation-cascade.csv" --output "$run/results"

for output in calibration-labels.csv exploratory-labels.csv exploratory-summary.json exploratory-recipe-agent.csv \
    confirmation-labels.csv confirmation-summary.json confirmation-recipe-agent.csv; do
    cmp -- "$here/results/$output" "$run/results/$output"
done
