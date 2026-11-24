import argparse
import os
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Build the WSL refill comparator")
    parser.add_argument("solver", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("--simulate-source", type=Path, default=Path(__file__).resolve().parents[2] / "match3_difficulty/simulate.cpp")
    args = parser.parse_args()
    old = args.simulate_source.read_text(encoding="utf-8")
    marker = "private:\n    const PolicyChoice& choices(std::size_t index)"
    if old.count(marker) != 1:
        raise ValueError("Unexpected archived policy implementation")
    accessor = "    const PolicyChoice& refill_choices(std::size_t index) { return choices(index); }\n\n"
    args.build.mkdir(parents=True, exist_ok=True)
    (args.build / "simulate_for_refill.inc").write_text(old.replace(marker, accessor + marker), encoding="utf-8")
    source = Path(__file__).resolve().parent / "refill_experiment.cpp"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Wpedantic", "-pthread",
        "-I", str(args.build), "-I", str(args.solver / "include"), "-I", str(args.solver / "src"),
        str(source), str(args.library), "-o", str(args.build / "refill-experiment"),
    ], check=True)


if __name__ == "__main__":
    main()
