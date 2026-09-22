"""Read-only offline audit of the exact TSV or Parquet file to be republished."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path

import duckdb

from atx_db.ticker_history_quality import source_diagnostics, source_provenance, stage_source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-path", "--tsv-path", dest="source_path", required=True, type=Path,
                        help="Local TSV/text or Parquet source; --tsv-path is a compatibility alias")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--memory-limit", default="1GB")
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    started = time.perf_counter()
    provenance = source_provenance(args.source_path)
    initial = args.source_path.stat()
    with args.source_path.open("rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    sidecar = args.source_path.with_suffix(args.source_path.suffix + ".sha256")
    declared = sidecar.read_text(encoding="utf-8").strip() if sidecar.exists() else None
    if declared is not None and digest != declared:
        raise ValueError("source SHA256 differs from source sidecar")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    spill = args.output.parent / (args.output.stem + "-spill")
    with duckdb.connect() as con:
        con.execute("SET threads = 1")
        con.execute("SET memory_limit = ?", [args.memory_limit])
        con.execute("SET temp_directory = ?", [str(spill)])
        con.execute("SET preserve_insertion_order = false")
        stage_source(con, str(args.source_path))
        diagnostics = source_diagnostics(con)
    final = args.source_path.stat()
    if (initial.st_size, initial.st_mtime_ns) != (final.st_size, final.st_mtime_ns):
        raise RuntimeError("source changed during audit")
    report = {"source_path": str(args.source_path.resolve()), "source_bytes": initial.st_size,
              "source_format": provenance["source_format"],
              "source_sha256": digest, "sidecar_sha256": declared,
              "hash_scope": "full source file before audit; size and modification time checked after",
              "diagnostics": diagnostics, "provenance": provenance,
              "elapsed_seconds": time.perf_counter() - started,
              "diagnostic_bins": "absolute residual magnitudes, not economic acceptance thresholds"}
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, sort_keys=True, default=str)
        handle.write("\n")
    print(json.dumps(report, indent=2, sort_keys=True, default=str))


if __name__ == "__main__":
    main()
