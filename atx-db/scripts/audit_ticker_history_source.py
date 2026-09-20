"""Read-only offline audit of the exact TSV that will be republished."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path

import duckdb

from atx_db.ticker_history_quality import SOURCE_PROVENANCE, source_diagnostics, stage_source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tsv-path", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--memory-limit", default="1GB")
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    started = time.perf_counter()
    initial = args.tsv_path.stat()
    with args.tsv_path.open("rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    sidecar = args.tsv_path.with_suffix(args.tsv_path.suffix + ".sha256")
    declared = sidecar.read_text(encoding="utf-8").strip() if sidecar.exists() else None
    if declared is not None and digest != declared:
        raise ValueError("source SHA256 differs from extraction sidecar")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    spill = args.output.parent / (args.output.stem + "-spill")
    with duckdb.connect() as con:
        con.execute("SET threads = 1")
        con.execute("SET memory_limit = ?", [args.memory_limit])
        con.execute("SET temp_directory = ?", [str(spill)])
        con.execute("SET preserve_insertion_order = false")
        stage_source(con, str(args.tsv_path))
        diagnostics = source_diagnostics(con)
    final = args.tsv_path.stat()
    if (initial.st_size, initial.st_mtime_ns) != (final.st_size, final.st_mtime_ns):
        raise RuntimeError("source changed during audit")
    report = {"source_path": str(args.tsv_path.resolve()), "source_bytes": initial.st_size,
              "source_sha256": digest, "sidecar_sha256": declared,
              "hash_scope": "full TSV before audit; size and modification time checked after",
              "diagnostics": diagnostics, "provenance": SOURCE_PROVENANCE,
              "elapsed_seconds": time.perf_counter() - started,
              "diagnostic_bins": "absolute residual magnitudes, not economic acceptance thresholds"}
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True, default=str)
        handle.write("\n")
    print(json.dumps(report, indent=2, sort_keys=True, default=str))


if __name__ == "__main__":
    main()
