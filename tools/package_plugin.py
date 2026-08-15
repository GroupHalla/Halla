#!/usr/bin/env python3
"""Create a reproducible .halla-addon ZIP and its SHA-256 file."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

MAX_PACKAGE_SOURCE = 250 * 1024 * 1024
MAX_FILES = 2000


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="directory containing manifest.json")
    parser.add_argument("output", type=Path, help="destination ending in .halla-addon")
    args = parser.parse_args()

    source = args.source.resolve()
    manifest_path = source / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit("manifest.json not found")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plugin_id = manifest.get("id", "")
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{2,63}", plugin_id):
        raise SystemExit("invalid plugin id")
    if manifest.get("type") != "native" or manifest.get("apiVersion") != 1:
        raise SystemExit("manifest must use native API version 1")

    files = sorted(path for path in source.rglob("*") if path.is_file())
    if not files or len(files) > MAX_FILES:
        raise SystemExit("invalid number of files")
    total = 0
    for path in files:
        if path.is_symlink():
            raise SystemExit(f"symbolic links are not allowed: {path}")
        total += path.stat().st_size
    if total > MAX_PACKAGE_SOURCE:
        raise SystemExit("package source exceeds 250 MiB")

    output = args.output.resolve()
    if output.suffix.lower() != ".halla-addon":
        raise SystemExit("output must end in .halla-addon")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            relative = path.relative_to(source).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes())

    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    checksum = output.with_name(output.name + ".sha256")
    checksum.write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(f"created {output}")
    print(f"SHA-256 {digest}")


if __name__ == "__main__":
    main()
