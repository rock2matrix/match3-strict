import argparse
import os
import subprocess
from pathlib import Path


def build(solver, library, build_dir, source_name="acr_benchmark.cpp", target="acr-benchmark"):
    here = Path(__file__).resolve().parent
    work = here.parent
    build_dir.mkdir(parents=True, exist_ok=True)
    original = (work / "match3_difficulty/simulate.cpp").read_text(encoding="utf-8")
    marker = "private:\n    const PolicyChoice& choices(std::size_t index)"
    if original.count(marker) != 1:
        raise ValueError("Prior policy definition changed")
    accessor = "    const PolicyChoice& refill_choices(std::size_t index) { return choices(index); }\n\n"
    (build_dir / "simulate_for_acr.inc").write_text(original.replace(marker, accessor + marker), encoding="utf-8")
    previous = (work / "match3_v4/refill/refill_experiment.cpp").read_text(encoding="utf-8")
    previous = previous.replace('"simulate_for_refill.inc"', '"simulate_for_acr.inc"')
    previous = previous.replace('"refill_oracle.hpp"', '"refill_oracle_for_acr.inc"')
    previous = previous[:previous.index("\nstruct BoardOutput {")] + "\n} // namespace\n"
    previous = previous[previous.index('#include "simulate_for_acr.inc"'):]
    previous = previous.replace("#undef main", "#pragma pop_macro(\"main\")", 1)
    previous = '#pragma push_macro("main")\n#undef main\n#define main old_simulator_main\n' + previous
    (build_dir / "refill_for_acr.inc").write_text(previous, encoding="utf-8")
    oracle = (work / "match3_v4/refill/refill_oracle.hpp").read_text(encoding="utf-8")
    marker = "private:\n    enum class Stop"
    if oracle.count(marker) != 1:
        raise ValueError("Prior refill oracle definition changed")
    oracle = oracle.replace(marker, "private:\n    friend struct OfflineBankAccess;\n    enum class Stop")
    (build_dir / "refill_oracle_for_acr.inc").write_text(oracle, encoding="utf-8")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
        "-I", str(build_dir), "-I", str(here), "-I", str(solver / "include"), "-I", str(solver / "src"),
        str(here / source_name), str(here / "acr_controller.cpp"), str(here / "safety_mass_estimator.cpp"),
        str(library), "-o", str(build_dir / target),
    ], check=True)


def main():
    parser = argparse.ArgumentParser(description="Build the exploratory ACR offline/hybrid benchmark in WSL")
    parser.add_argument("solver", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("--source", default="acr_benchmark.cpp")
    parser.add_argument("--target", default="acr-benchmark")
    args = parser.parse_args()
    build(args.solver, args.library, args.build, args.source, args.target)


if __name__ == "__main__":
    main()
