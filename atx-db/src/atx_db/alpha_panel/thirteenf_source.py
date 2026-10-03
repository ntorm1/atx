"""Bounded retained-source discovery; metadata is never a completeness certificate.

No network or legacy archive imports. Explicit roots include configured overrides;
unresolved historical CLI overrides must remain release exceptions.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
from typing import Any, TextIO

from . import common as C


def configured_roots() -> dict[str, str]:
    package = Path(__file__).resolve().parents[3]
    configured = os.environ.get("ATX_DB_DATA_DIR")
    database = os.environ.get("ATX_DB_PATH")
    warehouse = (Path(os.path.expandvars(configured)).expanduser() if configured else
                 Path(os.path.expandvars(database)).expanduser().parent if database else package / "data")
    return {
        "repository_data": str(package.parent / "data"),
        "package_data": str(package / "data"),
        "resolved_warehouse_data": str(warehouse),
        "ATX_ALPHA_PANEL_ROOT": os.environ.get("ATX_ALPHA_PANEL_ROOT", str(C.DEFAULT_ROOT)),
    }


def audit_roots(roots: list[Path], *, max_files: int = 200000,
                max_depth: int = 16, metadata_sink: TextIO | None = None) -> dict[str, Any]:
    """Stream directory metadata, skip links/reparse points, stop on explicit bounds.

    Every regular file is counted even without a 13F-looking name. Candidate
    paths are retained separately for subsequent guarded content qualification.
    Permission failures, bounds and unresolved roots forbid completeness claims.
    """
    if max_files < 1 or max_depth < 1:
        raise ValueError("positive traversal bounds required")
    result: dict[str, Any] = {"schema": "atx.thirteenf-source-roots/v1",
        "available_at": dt.datetime.now(dt.UTC).isoformat(), "roots": [],
        "candidate_files": [], "exceptions": [], "files": 0, "bytes": 0,
        "source_completeness": "UNMEASURED", "unresolved_historical_cli_overrides": True}
    seen: set[str] = set()
    for root in roots:
        absolute = root.absolute()
        key = os.path.normcase(str(absolute))
        if key in seen:
            continue
        seen.add(key)
        record = {"path": str(absolute), "exists": absolute.exists(), "files": 0, "bytes": 0}
        result["roots"].append(record)
        if not record["exists"]:
            result["exceptions"].append({"path": str(absolute), "reason": "missing_root"})
            continue
        stack = [(absolute, 0)]
        while stack:
            directory, depth = stack.pop()
            try:
                with os.scandir(directory) as entries:
                    for entry in entries:
                        info = entry.stat(follow_symlinks=False)
                        # FILE_ATTRIBUTE_REPARSE_POINT includes Windows junctions.
                        if entry.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                            result["exceptions"].append({"path": entry.path, "reason": "reparse_point_skipped"})
                            continue
                        if entry.is_dir(follow_symlinks=False):
                            if depth >= max_depth:
                                result["exceptions"].append({"path": entry.path, "reason": "depth_bound"})
                            else:
                                stack.append((Path(entry.path), depth + 1))
                        elif entry.is_file(follow_symlinks=False):
                            if result["files"] >= max_files:
                                result["exceptions"].append({"path": entry.path, "reason": "file_bound"})
                                result["metadata_complete"] = False
                                return result
                            result["files"] += 1
                            result["bytes"] += info.st_size
                            record["files"] += 1
                            record["bytes"] += info.st_size
                            if metadata_sink is not None:
                                metadata_sink.write(json.dumps({"path": entry.path,
                                    "bytes": info.st_size, "mtime_ns": info.st_mtime_ns}) + "\n")
                            name = entry.path.lower().replace("-", "_")
                            if "13f" in name or "thirteenf" in name:
                                result["candidate_files"].append({"path": entry.path,
                                    "bytes": info.st_size, "mtime_ns": info.st_mtime_ns,
                                    "kind": "receipt" if entry.name.endswith("jsonl") else "unqualified"})
            except OSError as exc:
                result["exceptions"].append({"path": str(directory), "reason": type(exc).__name__})
    result["metadata_complete"] = not result["exceptions"]
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    audit = sub.add_parser("audit-roots")
    audit.add_argument("--data-root", type=Path, action="append", required=True)
    audit.add_argument("--output", type=Path, required=True)
    audit.add_argument("--max-files", type=int, default=200000)
    audit.add_argument("--max-depth", type=int, default=16)
    audit.add_argument("--file-inventory", type=Path,
                       help="Streaming JSONL metadata for every file, including opaque names")
    schema = sub.add_parser("schemas")
    schema.add_argument("--file", type=Path, action="append", required=True)
    schema.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.error("audit output exists; choose a new evidence path")
    if args.command == "schemas":
        if len(args.file) > 8:
            parser.error("at most eight explicit source files")
        con = C.connect(memory="64MB", threads=1)
        try:
            records = []
            for path in args.file:
                escaped = path.as_posix().replace("'", "''")
                rows = con.execute(f"DESCRIBE SELECT * FROM read_parquet('{escaped}')").fetchall()
                records.append({"file": C.file_identity(path), "columns": rows})
            C.write_json_atomic(args.output, {"schema": "atx.thirteenf-source-schemas/v1", "files": records})
        finally:
            con.close()
        return 0
    if args.file_inventory:
        if args.file_inventory.exists():
            parser.error("file inventory exists; choose a new evidence path")
        args.file_inventory.parent.mkdir(parents=True, exist_ok=True)
        # Open exclusively: never overwrite prior audit evidence.
        with args.file_inventory.open("x", encoding="utf-8") as sink:
            result = audit_roots(args.data_root, max_files=args.max_files,
                                 max_depth=args.max_depth, metadata_sink=sink)
        result["file_inventory"] = str(args.file_inventory)
        result["file_inventory_sha256"] = C.sha256_file(args.file_inventory)
        result["file_inventory_bytes"] = args.file_inventory.stat().st_size
    else:
        result = audit_roots(args.data_root, max_files=args.max_files, max_depth=args.max_depth)
    result["configured_roots"] = configured_roots()
    C.write_json_atomic(args.output, result)
    return 0 if result["metadata_complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
