#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'Usage: bash run.sh MATCH3_STRICT_CHECKOUT NEW_RUN_DIRECTORY\n' >&2
    exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(realpath -- "$1")
run=$(realpath -m -- "$2")
commit=71aac8aab2e3fae2fa798e075f0f7d52b1220754

if [[ $(git -C "$repo" rev-parse HEAD) != "$commit" ]] ||
    ! git -C "$repo" diff --quiet HEAD --; then
    printf 'Use a clean source checkout at %s to reproduce these results.\n' "$commit" >&2
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

for program in generate simulate audit; do
    "${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -pthread \
        -I "$repo/include" -I "$repo/src" "$script_dir/$program.cpp" \
        "$run/build/libmatch3_strict.a" -o "$run/build/$program"
done

"$run/build/generate" "$run/dataset"
"$run/build/simulate" "$run/dataset" "$run/results"
"$run/build/audit" "$run/dataset" "$run/results/safe_mass_audit.csv" 8 \
    > "$run/results/safe_mass_audit_groups.csv"
python3 "$script_dir/analyze.py" "$run/dataset/candidates.csv" "$run/results/levels.csv" \
    "$run/results/policies.csv" --out-dir "$run/results"
python3 "$script_dir/select_pilot.py" --dataset "$run/dataset" --results "$run/results"
