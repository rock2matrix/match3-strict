#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 ]]; then
    printf 'Usage: bash reproduce.sh SOLVER_CHECKOUT NEW_RUN_DIRECTORY\n' >&2
    exit 2
fi

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
solver=$(realpath -- "$1")
run=$(realpath -m -- "$2")
commit=71aac8aab2e3fae2fa798e075f0f7d52b1220754

if [[ $(git -C "$solver" rev-parse HEAD) != "$commit" ]] ||
    ! git -C "$solver" diff --quiet HEAD --; then
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
calibration="$here/calibration"

"${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -I "$solver/include" \
    "$here/generate.cpp" "$library" -o "$run/build/generate"
"${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -I "$solver/include" -I "$solver/src" \
    "$calibration/simulate.cpp" "$library" -o "$run/build/simulate"
python3 "$here/build_exact.py" "$solver" "$library" "$run/build" \
    --simulate-source "$calibration/simulate.cpp"

python3 "$here/tiering.py" freeze "$calibration/candidates.csv" "$calibration/policies.csv" \
    "$run/frozen-standard.json" "$run/calibration-levels.csv"
cmp -- "$here/frozen-standard.json" "$run/frozen-standard.json"

"$run/build/generate" "$run/dataset" 50 4000
"$run/build/simulate" "$run/dataset" "$run/holdout_sim"
"$run/build/exact-features" "$run/dataset" "$run/holdout_sim/exact_features.csv"
python3 "$here/tiering.py" validate "$run/dataset/candidates.csv" "$run/holdout_sim/policies.csv" \
    "$run/frozen-standard.json" "$run/holdout"

for tier in easy normal hard; do
    python3 "$here/tiering.py" fill "$run/frozen-standard.json" "$run/build/generate" \
        "$run/build/exact-features" "$run/production/$tier" "$tier"
done

python3 "$here/mixed_fill.py" "$run/frozen-standard.json" "$here/mixed-plan.json" \
    "$run/build/generate" "$run/build/exact-features" "$run/production/mixed"
