import argparse
import os
import subprocess
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one matching ACR source fragment: {old!r}")
    return text.replace(old, new, 1)


def write_source(path, text):
    path.write_text(text, encoding="utf-8", newline="\n")


def prepare_headers(work, build_dir):
    simulator = (work / "match3_difficulty/simulate.cpp").read_text(encoding="utf-8")
    simulator = replace_once(
        simulator,
        "private:\n    const PolicyChoice& choices(std::size_t index)",
        "    const PolicyChoice& refill_choices(std::size_t index) { return choices(index); }\n\n"
        "private:\n    const PolicyChoice& choices(std::size_t index)",
    )
    write_source(build_dir / "simulate_for_acr.inc", simulator)

    old_refill = (work / "match3_v4/refill/refill_experiment.cpp").read_text(encoding="utf-8")
    old_refill = replace_once(old_refill, '"simulate_for_refill.inc"', '"simulate_for_acr.inc"')
    old_refill = replace_once(old_refill, '"refill_oracle.hpp"', '"refill_oracle_for_acr.inc"')
    old_refill = old_refill[old_refill.index('#include "simulate_for_acr.inc"'):old_refill.index("\nstruct BoardOutput {")]
    old_refill = replace_once(old_refill, "#undef main", "#pragma pop_macro(\"main\")")
    # The old driver is not used by either ACR benchmark. Retain its shared
    # counters, agents, policy and exact witness computation only.
    first = old_refill.index("\nvoid record_proposal(")
    last = old_refill.index("\ndouble exact_productive(")
    old_refill = old_refill[:first] + old_refill[last:]
    write_source(build_dir / "refill_for_acr.inc",
                 '#pragma push_macro("main")\n#undef main\n#define main old_simulator_main\n' + old_refill + "\n} // namespace\n")

    oracle = (work / "match3_v4/refill/refill_oracle.hpp").read_text(encoding="utf-8")
    oracle = replace_once(oracle, "private:\n    enum class Stop",
                          "private:\n    friend struct OfflineBankAccess;\n    friend struct ProofBankExpansionAccess;\n    enum class Stop")
    write_source(build_dir / "refill_oracle_for_acr.inc", oracle)

    acr = (work / "match3_acr/acr_benchmark.cpp").read_text(encoding="utf-8")
    acr = acr[:acr.index("\nint main(int argc")]
    acr = replace_once(
        acr,
        "AcrBoardOutput evaluate_acr_board(const fs::path& dataset, const BoardSpec& board, int runs,\n"
        "                                  std::uint64_t sample_seed, bool warm, const std::vector<ControllerSpec>& controllers)\n"
        "{\n    const auto prepared = prepare_board(dataset, board, sample_seed);",
        "AcrBoardOutput evaluate_prepared_board(const PreparedBoard* prepared, int runs,\n"
        "                                       std::uint64_t sample_seed, bool warm, const std::vector<ControllerSpec>& controllers)\n"
        "{\n    const auto& board = prepared->board;",
    )
    acr = replace_once(acr, "              << planned_witness << '\\n';",
                       "              << planned_witness << ',' << wave_text(state.board.cells)\n"
                       "              << ',' << std::hex << std::setfill('0') << std::setw(16) << hash_state(Bank::key(state))\n"
                       "              << ',' << std::setw(16)\n"
                       "              << hash_state(Bank::key(state) + static_cast<char>(witness.swap.a)\n"
                       "                            + static_cast<char>(witness.swap.b)) << std::dec << std::setfill(' ') << '\\n';")
    write_source(build_dir / "acr_for_proofbank.inc", acr)


def main():
    parser = argparse.ArgumentParser(description="Build the proof-bank comparison without changing prior ACR source")
    parser.add_argument("solver", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    work = here.parent
    build_dir = args.build.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    prepare_headers(work, build_dir)
    modules = work / "match3_acr"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
        "-I", str(build_dir), "-I", str(here), "-I", str(modules),
        "-I", str(args.solver / "include"), "-I", str(args.solver / "src"),
        str(here / "proofbank_benchmark.cpp"), str(modules / "acr_controller.cpp"),
        str(modules / "safety_mass_estimator.cpp"), str(args.library), "-o", str(build_dir / "proofbank-benchmark"),
    ], check=True)


if __name__ == "__main__":
    main()
