"""Build the cascade scorer against an immutable previous simulator and solver."""

import argparse
import os
import subprocess
from pathlib import Path


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("solver", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("--simulate-source", type=Path, default=here.parent / "match3_difficulty/simulate.cpp")
    args = parser.parse_args()

    source = args.simulate_source.read_text(encoding="utf-8")
    marker = "private:\n    const PolicyChoice& choices(std::size_t index)"
    if source.count(marker) != 1:
        raise ValueError("Prior simulator does not match the expected policy interface")
    accessor = "    const PolicyChoice& cascade_choices(std::size_t index) { return choices(index); }\n\n"
    args.build.mkdir(parents=True, exist_ok=True)
    (args.build / "simulate_for_cascade.inc").write_text(source.replace(marker, accessor + marker), encoding="utf-8")
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-O3", "-Wall", "-Wextra", "-Wpedantic",
        "-I", str(args.build), "-I", str(args.solver / "include"), "-I", str(args.solver / "src"),
        str(here / "cascade_features.cpp"), str(args.library), "-o", str(args.build / "cascade-features"),
    ], check=True)


if __name__ == "__main__":
    main()
