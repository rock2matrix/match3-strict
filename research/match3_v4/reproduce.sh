#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || ( $# -eq 3 && $3 != --pilot ) ]]; then
    printf 'Usage: bash reproduce.sh SOLVER_CHECKOUT NEW_RUN_DIRECTORY [--pilot]\n' >&2
    exit 2
fi

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(dirname -- "$here")
repo=$(realpath -- "$1")
run=$(realpath -m -- "$2")
commit=71aac8aab2e3fae2fa798e075f0f7d52b1220754
if [[ $(git -C "$repo" rev-parse HEAD) != "$commit" ]] || ! git -C "$repo" diff --quiet HEAD --; then
    printf 'Use the clean solver checkout at %s.\n' "$commit" >&2
    exit 2
fi
if [[ -e $run ]]; then
    printf 'Run directory already exists: %s\n' "$run" >&2
    exit 2
fi
mkdir -p -- "$run"
cmake -S "$repo" -B "$run/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$run/build" --config Release --parallel 4
ctest --test-dir "$run/build" -C Release --output-on-failure

python3 "$here/v4/run_v4.py" --work "$work" --solver "$repo" --verifier "$run/build/match3-solve" --output "$run/v4"
for name in summary.json calibration-labels.csv comparison-labels.csv recipe-agent.csv feature-overlap.csv partial-rank.csv; do
    cmp -- "$here/v4/results/$name" "$run/v4/$name"
done

refill="$here/refill"
fresh="$work/match3_ablation/fresh"
per_recipe=50
reference=full
if [[ ${3:-} == --pilot ]]; then
    per_recipe=8
    reference=pilot
fi

python3 "$refill/build_refill.py" "$repo" "$run/build/libmatch3_strict.a" "$run/build"
"$run/build/refill-experiment" "$fresh/dataset" "$run/refill" 64 "$per_recipe" 8 6 202611244
python3 "$refill/analyze_refill.py" "$run/refill/summary.csv" "$fresh/confirmation-cascade.csv" \
    "$refill/model-cuts.json" "$run/refill/analysis" --episodes "$run/refill/episodes.csv" \
    --witness-exact "$run/refill/witness-exact.csv" --oracle "$run/refill/oracle-board.csv"

for name in summary.csv episodes.csv witness-exact.csv wave-sizes.csv analysis/controller-summary.json \
    analysis/controller-board-scores.csv analysis/controller-rank-by-recipe.csv analysis/controller-agent-by-recipe.csv; do
    cmp -- "$refill/$reference/$name" "$run/refill/$name"
done
