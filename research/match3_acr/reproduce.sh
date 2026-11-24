#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || ( $# -eq 3 && $3 != --smoke ) ]]; then
    printf 'Usage: bash reproduce.sh SOLVER_CHECKOUT NEW_RUN_DIRECTORY [--smoke]\n' >&2
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

"${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -Wconversion -Werror -I "$here" \
    "$here/controller_smoke.cpp" "$here/acr_controller.cpp" "$here/safety_mass_estimator.cpp" -o "$run/build/controller-smoke"
"$run/build/controller-smoke"
python3 "$here/build_benchmark.py" "$solver" "$library" "$run/build"
python3 "$here/build_benchmark.py" "$solver" "$library" "$run/build" --source acr_load.cpp --target acr-load

cohort="$work/match3_ablation/fresh/dataset"
episodes=64
quota=50
if [[ ${3:-} == --smoke ]]; then
    episodes=8
    quota=2
fi
"$run/build/acr-benchmark" "$cohort" "$run/m3-cold" "$episodes" "$quota" 8 202611245 cold all
"$run/build/acr-benchmark" "$cohort" "$run/m3-warm" "$episodes" "$quota" 8 202611245 warm all
python3 "$here/validate_benchmark.py" "$run/m3-warm" "$run/m3-cold" "$run/m3-validation.json"
if [[ ${3:-} == --smoke ]]; then
    exit 0
fi

python3 "$here/analyze_acr.py" "$run/m3-cold" "$run/m3-warm" \
    "$work/match3_ablation/results/confirmation-labels.csv" "$run/analysis"
"$run/build/acr-load" "$cohort" "$run/load" 4 202611246 2
python3 "$here/check_acr_load.py" "$run/load"

"$run/build/acr-benchmark" "$work/match3_difficulty/dataset" "$run/m5-cal" 64 25 8 202611246 warm adaptive
python3 "$here/m5_calibration.py" calibrate "$run/m5-cal/summary.csv" "$work/match3_pool/frozen-standard.json" "$run/m5-analysis"
"$run/build/acr-benchmark" "$work/match3_pool/dataset" "$run/m5-validation" 64 50 8 202611246 warm adaptive
python3 "$here/m5_calibration.py" validate "$run/m5-analysis/acr-standard-lock.json" "$run/m5-validation/summary.csv" \
    "$cohort/candidates.csv" "$work/match3_pool/validation/holdout-levels.csv" "$run/m5-analysis"
