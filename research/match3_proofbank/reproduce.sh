#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 || ( $# -eq 3 && $3 != --smoke ) ]]; then
    printf 'Usage: bash reproduce.sh SOLVER_CHECKOUT NEW_RUN_DIRECTORY [--smoke]\n' >&2
    exit 2
fi
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(dirname -- "$here")
repo=$(realpath -- "$1")
run=$(realpath -m -- "$2")
commit=71aac8aab2e3fae2fa798e075f0f7d52b1220754
if [[ $(git -C "$repo" rev-parse HEAD) != "$commit" ]] || ! git -C "$repo" diff --quiet HEAD --; then
    printf 'Use the clean pinned solver checkout at %s.\n' "$commit" >&2
    exit 2
fi
if [[ -e $run ]]; then
    printf 'Choose a new run directory: %s\n' "$run" >&2
    exit 2
fi
mkdir -p -- "$run"
cmake -S "$repo" -B "$run/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$run/build" --config Release --parallel 4
ctest --test-dir "$run/build" -C Release --output-on-failure
library="$run/build/libmatch3_strict.a"
python3 "$here/build_proofbank.py" "$repo" "$library" "$run/build"
"${CXX:-c++}" -std=c++20 -O3 -Wall -Wextra -Wpedantic -I "$repo/include" -I "$repo/src" \
    "$here/audit_proof_bank.cpp" "$library" -o "$run/build/audit-proof-bank"

dataset="$work/match3_ablation/fresh/dataset"
cohort="$work/match3_pool"
cal="$work/match3_difficulty/dataset"
prior="$work/match3_acr/results/m5-analysis"
bench="$run/build/proofbank-benchmark"
episodes=64
boards=50
if [[ ${3:-} == --smoke ]]; then
    episodes=16
    boards=2
fi

"$bench" "$dataset" "$run/base-cold" "$episodes" "$boards" 8 202611249 cold base pair 0 noarchive
"$bench" "$dataset" "$run/targeted-cold" "$episodes" "$boards" 8 202611249 cold targeted pair 240 noarchive
"$bench" "$dataset" "$run/targeted-warm" "$episodes" "$boards" 8 202611249 warm targeted pair 240 archive
"$bench" "$dataset" "$run/base-warm" "$episodes" "$boards" 8 202611249 warm base pair 0 noarchive

python3 "$here/audit_proof_bank.py" seal "$run/targeted-warm/bank-entries" "$dataset" "$run/bank-audit" \
    --verifier "$run/build/audit-proof-bank" --solver-cli "$run/build/match3-solve" --solver-checkout "$repo" \
    --expansion-csv "$run/targeted-warm/expansion.csv" --expansion-count-field newly_verified \
    --source "$here/protocol.json" --source "$here/expansion.hpp" --source "$here/proofbank_proofs.hpp" \
    --source "$here/proofbank_benchmark.cpp" --source "$here/build_proofbank.py" \
    --source "$work/match3_acr/acr_controller.cpp" --source "$work/match3_acr/safety_mass_estimator.cpp"
python3 "$here/audit_proof_bank.py" verify "$run/bank-audit/proof-bank-manifest.json" \
    "$run/targeted-warm/bank-entries" "$dataset"

if [[ ${3:-} == --smoke ]]; then
    exit 0
fi

python3 "$here/analyze_expansion.py" "$run/base-cold" "$run/base-warm" \
    "$run/targeted-cold" "$run/targeted-warm" "$run/analysis"
"$bench" "$cal" "$run/m5-cal" 64 25 8 202611250 warm targeted adaptive 240 noarchive
python3 "$here/calibrate_v11.py" calibrate "$run/m5-cal/summary.csv" "$cal/candidates.csv" \
    "$cohort/frozen-standard.json" "$prior/acr-standard-lock.json" "$run/m5-analysis"
"$bench" "$cohort/dataset" "$run/m5-validation" 64 50 8 202611250 warm targeted adaptive 240 noarchive
python3 "$here/calibrate_v11.py" validate "$run/m5-analysis/acr-v11-standard-lock.json" \
    "$run/m5-validation/summary.csv" "$cohort/dataset/candidates.csv" "$dataset/candidates.csv" \
    "$cohort/validation/holdout-levels.csv" "$prior/validation-levels.csv" "$run/m5-analysis"
