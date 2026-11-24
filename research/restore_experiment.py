#!/usr/bin/env python3
"""Verify a released experiment archive and restore it to a new directory."""

import argparse
import hashlib
import json
import shutil
import stat
import tempfile
from pathlib import Path, PurePosixPath
from zipfile import ZipFile


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def restore(asset, destination, item):
    if asset.stat().st_size != item["bytes"] or digest(asset) != item["sha256"]:
        raise ValueError("archive size or SHA-256 does not match artifacts.json")
    destination = destination.resolve()
    if destination.exists():
        raise FileExistsError(f"Choose a new destination: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    prefix = item["root"] + "/"
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".match3-restore-") as temporary:
        payload = Path(temporary) / "payload"
        payload.mkdir()
        with ZipFile(asset) as archive:
            seen = set()
            for entry in archive.infolist():
                name = entry.filename
                relative = name.removeprefix(prefix)
                path = PurePosixPath(relative)
                file_type = stat.S_IFMT(entry.external_attr >> 16)
                if (not name.startswith(prefix) or path.is_absolute() or ".." in path.parts or
                        "\\" in name or file_type not in (0, stat.S_IFDIR, stat.S_IFREG)):
                    raise ValueError(f"Unexpected archive member: {name}")
                if entry.is_dir():
                    continue
                if not relative or path in seen:
                    raise ValueError(f"Repeated archive member: {name}")
                seen.add(path)
                output = payload.joinpath(*path.parts)
                output.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, output.open("xb") as target:
                    shutil.copyfileobj(source, target, 1024 * 1024)
        required = ["SHA256SUMS", "solver/match3-strict.bundle", *item["entrypoints"]]
        for name in required:
            if not (payload / name).is_file():
                raise ValueError(f"Archive lacks {name}")
        if destination.exists():
            raise FileExistsError(f"Destination appeared during restore: {destination}")
        payload.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source = Path(__file__).with_name("artifacts.json")
    items = json.loads(source.read_text(encoding="utf-8"))["artifacts"]
    item = next((item for item in items if item["file"] == args.archive.name), None)
    if item is None:
        parser.error("The archive filename is not listed in artifacts.json")
    restore(args.archive, args.destination, item)
    print(f"Verified and restored {item['file']} to {args.destination.resolve()}")


if __name__ == "__main__":
    main()
