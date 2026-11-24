#!/usr/bin/env python3
"""Check that malformed replay records cannot enter a sealed proof bank."""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("checker", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    with args.archive.open("rb") as source:
        metadata = source.readline().decode("ascii").split()
        payload = source.read(int(metadata[-1]))
    assert len(payload) == int(metadata[-1])

    def frame(fields, content):
        return (" ".join(fields) + "\n").encode("ascii") + content

    positive = frame(metadata, payload)
    same_cell_swap = list(metadata)
    same_cell_swap[12] = same_cell_swap[11]
    changed_quota = list(metadata)
    first = changed_quota[14]
    known_color = int(first[:2], 16)
    different_color = (known_color + 1) % int(metadata[7])
    changed_quota[14] = f"{different_color:02x}" + first[2:]

    tests = {
        "valid_replay": (positive, None),
        "illegal_same_cell_swap": (frame(same_cell_swap, payload), "not a legal swap"),
        "bad_refill_quota": (frame(changed_quota, payload), "independent replay disagrees"),
        "truncated_certificate": (positive[:-1], "truncated certificate payload"),
        "malformed_certificate": (frame(metadata, b"!" + payload[1:]), "certificate failed independent public verification"),
        "duplicated_record_ordinal": (positive + positive, "duplicate record ordinal"),
    }
    results = {}
    with tempfile.TemporaryDirectory(prefix="proof-bank-negative-tests-") as temporary:
        scratch = Path(temporary)
        for test, (input_data, expected_error) in tests.items():
            records = scratch / test / "entries"
            records.mkdir(parents=True)
            (records / args.archive.name).write_bytes(input_data)
            result = subprocess.run([args.checker, records, records.parent / "samples"], check=False,
                                    capture_output=True, text=True)
            if expected_error is None:
                assert result.returncode == 0, (test, result.stderr)
            else:
                assert result.returncode != 0 and expected_error in result.stderr, (test, result.stderr)
            results[test] = {"exit_code": result.returncode, "error": result.stderr.strip() or None}
    args.output.write_text(json.dumps({"level": metadata[1], "ordinal": int(metadata[2]), "tests": results},
                                      indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("Accepted the valid record and rejected all five invalid variants")


if __name__ == "__main__":
    main()
