"""Build the exact level scorer with checkpointed prior simulator semantics."""

import argparse
import os
import subprocess
from pathlib import Path


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("solver", type=Path, help="Match3 Strict source checkout")
    parser.add_argument("library", type=Path, help="Compiled libmatch3_strict.a")
    parser.add_argument("build", type=Path, help="Output directory")
    parser.add_argument("--simulate-source", type=Path, default=here.parent / "match3_difficulty/simulate.cpp")
    args = parser.parse_args()

    # Keep the previous simulator file unchanged. Add a single accessor to its
    # cached action selection for the supplemental expected-value traversal.
    source = args.simulate_source.read_text(encoding="utf-8")
    marker = "private:\n    const PolicyChoice& choices(std::size_t index)"
    if source.count(marker) != 1:
        raise ValueError("Prior simulator does not match the expected policy interface")
    adapter = (
        "    const PolicyChoice& pool_choices(std::size_t index) { return choices(index); }\n\n"
        + marker
    )
    args.build.mkdir(parents=True, exist_ok=True)
    (args.build / "simulate_for_pool.inc").write_text(source.replace(marker, adapter), encoding="utf-8")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Wpedantic",
        "-I", str(args.build), "-I", str(args.solver / "include"), "-I", str(args.solver / "src"),
        str(here / "exact_features.cpp"), str(args.library), "-o", str(args.build / "exact-features"),
    ], check=True)


if __name__ == "__main__":
    main()
