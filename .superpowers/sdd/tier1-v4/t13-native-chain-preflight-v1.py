"""Root-only guarded footer preflight; no rows, return payload, HTTP or full hashes.

Do not run outside run_memory_guarded.py. Exact paths only, no directory search.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path


def inspect(path: Path, reader: Path) -> dict:
    result = {"path": str(path), "native_reader": str(reader), "value_rows_read": 0,
              "full_file_hash_computed": False}
    if not path.exists():
        return {**result, "status": "MISSING_EXACT_PATH"}
    stat = path.stat()
    result.update({"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns})
    if stat.st_size < 12:
        return {**result, "status": "INVALID_PARQUET_SIZE"}
    with path.open("rb") as stream:
        stream.seek(-8, 2)
        tail = stream.read(8)
    footer = int.from_bytes(tail[:4], "little")
    result["footer_bytes"] = footer
    if tail[4:] != b"PAR1" or footer > 8 * 1024 * 1024 or footer + 12 > stat.st_size:
        return {**result, "status": "FOOTER_BOUND_OR_FORMAT_REJECTED"}
    import pyarrow.parquet as pq
    metadata = pq.read_metadata(path)
    result.update({"status": "METADATA_ONLY_UNQUALIFIED", "schema": str(metadata.schema.to_arrow_schema()),
                   "rows_metadata": metadata.num_rows, "row_groups": metadata.num_row_groups})
    return result


def main() -> int:
    if not os.environ.get("ATX_GUARD_JOB"):
        raise SystemExit("external root memory guard required")
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lake", type=Path, default=Path("C:/atx/atx-db/data/alpha_panel/v1"))
    ap.add_argument("--notes-source", action="append")
    ap.add_argument("--output", type=Path, default=Path("C:/atx/.superpowers/sdd/tier1-v4/task13-native-chain-preflight-v1.json"))
    args = ap.parse_args()
    if args.output.exists():
        raise SystemExit("refusing output overwrite")
    sources = args.notes_source or ["2019q4", "2020q1"]
    if len(sources) > 2 or any(not __import__("re").fullmatch(r"20\d{2}(?:q[1-4]|_\d{2})(?:_\d+)?", s) for s in sources):
        raise SystemExit("at most two exact notes-source keys")
    code = Path("C:/atx/atx-db/src/atx_db/alpha_panel")
    inputs = [(args.lake / "sec_filings/filings.parquet", code / "sec_filings.py"),
              (args.lake / "identity_cover/cover_page.parquet", code / "cover_page.py"),
              (args.lake / "corporate_actions/vendor_events.parquet", code / "corporate_actions.py"),
              (args.lake / "calendar.parquet", code / "common.py")]
    for source in sources:
        inputs.extend((args.lake / "notes/parts" / f"source={source}" / f"{table}.parquet", code / "notes_fetch.py")
                      for table in ("sub", "txt_dei", "num_dei", "dim"))
    if len(inputs) > 12:
        raise SystemExit("exact input file bound exceeded")
    output = {"status": "BOUNDED_METADATA_ONLY_UNQUALIFIED", "inputs": [inspect(p, r) for p, r in inputs],
              "max_files": 12, "footer_limit_bytes_per_file": 8388608,
              "value_rows_read": 0, "return_values_read_or_computed": False,
              "full_source_hashes_computed": False, "http_attempts": 0,
              "qualification": "Schema/footer leads only; not source semantics, class, currency, clock, cadence, history, denominator or task gate proof."}
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(output, stream, indent=2)
    print(json.dumps({"output": str(args.output), "files": len(inputs), "status": output["status"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
