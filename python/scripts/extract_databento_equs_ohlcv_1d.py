#!/usr/bin/env python
"""Extract Databento EQUS.SUMMARY ohlcv-1d DBN zip into date Hive Parquet."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import os
import re
import shutil
import sys
import tempfile
import zipfile
from dataclasses import asdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

try:
    import databento as db
except ImportError as exc:  # pragma: no cover - exercised by users without deps.
    raise SystemExit(
        "Missing dependency: databento. Install with `python -m pip install databento`."
    ) from exc


ENTRY_DATE_RE = re.compile(r"(?P<date>\d{8})\.ohlcv-1d\.dbn\.zst$", re.IGNORECASE)
DEFAULT_OUTPUT_ROOT = Path("data") / "databento" / "equs_ohlcv_1d_by_date"

OUTPUT_SCHEMA = pa.schema(
    [
        ("date", pa.date32()),
        ("symbol", pa.string()),
        ("instrument_id", pa.uint32()),
        ("ts_event", pa.timestamp("ns", tz="UTC")),
        ("open", pa.float64()),
        ("high", pa.float64()),
        ("low", pa.float64()),
        ("close", pa.float64()),
        ("volume", pa.uint64()),
        ("publisher_id", pa.uint16()),
        ("rtype", pa.uint8()),
        ("source_file", pa.string()),
    ]
)


@dataclass(frozen=True)
class EntryResult:
    entry: str
    date: str
    rows: int
    symbols: int
    output_file: str
    skipped: bool = False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Extract Databento EQUS.SUMMARY ohlcv-1d DBN files from a batch zip "
            "into a Hive-style Parquet dataset partitioned by date."
        )
    )
    parser.add_argument(
        "--zip-path",
        type=Path,
        help="Path to EQUS Databento zip. Defaults to newest EQUS-*.zip in Downloads.",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=DEFAULT_OUTPUT_ROOT,
        help=f"Output Hive dataset root. Default: {DEFAULT_OUTPUT_ROOT}",
    )
    parser.add_argument("--start-date", help="Optional inclusive YYYY-MM-DD filter.")
    parser.add_argument("--end-date", help="Optional inclusive YYYY-MM-DD filter.")
    parser.add_argument("--limit", type=int, help="Optional max number of date files to process.")
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Rewrite date partitions even when part-00000.parquet already exists.",
    )
    parser.add_argument(
        "--compression",
        default="zstd",
        choices=["zstd", "snappy", "gzip", "brotli", "none"],
        help="Parquet compression codec.",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Python logging level.",
    )
    return parser.parse_args()


def newest_equs_zip(downloads_dir: Path) -> Path:
    candidates = sorted(
        downloads_dir.glob("EQUS-*.zip"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError(f"No EQUS-*.zip files found in {downloads_dir}")
    return candidates[0]


def date_from_entry(name: str) -> dt.date | None:
    match = ENTRY_DATE_RE.search(name)
    if not match:
        return None
    return dt.datetime.strptime(match.group("date"), "%Y%m%d").date()


def parse_date_arg(value: str | None) -> dt.date | None:
    if not value:
        return None
    return dt.date.fromisoformat(value)


def list_dbn_entries(
    zip_path: Path,
    start_date: dt.date | None,
    end_date: dt.date | None,
    limit: int | None,
) -> list[tuple[str, dt.date]]:
    with zipfile.ZipFile(zip_path) as zf:
        entries: list[tuple[str, dt.date]] = []
        for info in zf.infolist():
            entry_date = date_from_entry(info.filename)
            if entry_date is None:
                continue
            if start_date and entry_date < start_date:
                continue
            if end_date and entry_date > end_date:
                continue
            entries.append((info.filename, entry_date))
    entries.sort(key=lambda item: item[1])
    return entries[:limit] if limit else entries


def output_file_for_date(output_root: Path, date_value: dt.date) -> Path:
    return output_root / f"date={date_value.isoformat()}" / "part-00000.parquet"


def normalize_ohlcv_frame(df: pd.DataFrame, date_value: dt.date, source_file: str) -> pd.DataFrame:
    if df.empty:
        return pd.DataFrame(columns=OUTPUT_SCHEMA.names)

    frame = df.reset_index()
    if "ts_event" not in frame.columns:
        first_column = frame.columns[0]
        frame = frame.rename(columns={first_column: "ts_event"})

    frame["date"] = date_value
    frame["source_file"] = source_file
    frame["ts_event"] = pd.to_datetime(frame["ts_event"], utc=True)

    if "symbol" not in frame.columns:
        frame["symbol"] = pd.NA

    return frame[OUTPUT_SCHEMA.names]


def write_parquet_atomic(frame: pd.DataFrame, destination: Path, compression: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    codec = None if compression == "none" else compression

    table = pa.Table.from_pandas(frame, schema=OUTPUT_SCHEMA, preserve_index=False)
    with tempfile.TemporaryDirectory(prefix=".tmp-", dir=destination.parent) as tmp_dir:
        tmp_path = Path(tmp_dir) / destination.name
        pq.write_table(table, tmp_path, compression=codec, use_dictionary=True)
        os.replace(tmp_path, destination)


def extract_entry(
    zip_path: Path,
    entry_name: str,
    date_value: dt.date,
    output_root: Path,
    overwrite: bool,
    compression: str,
) -> EntryResult:
    output_file = output_file_for_date(output_root, date_value)
    if output_file.exists() and output_file.stat().st_size > 0 and not overwrite:
        return EntryResult(
            entry=entry_name,
            date=date_value.isoformat(),
            rows=0,
            symbols=0,
            output_file=str(output_file),
            skipped=True,
        )

    with zipfile.ZipFile(zip_path) as zf:
        payload = zf.read(entry_name)

    store = db.DBNStore.from_bytes(payload)
    df = store.to_df(price_type="float", pretty_ts=True, map_symbols=True)
    frame = normalize_ohlcv_frame(df, date_value, entry_name)
    write_parquet_atomic(frame, output_file, compression)
    return EntryResult(
        entry=entry_name,
        date=date_value.isoformat(),
        rows=len(frame),
        symbols=int(frame["symbol"].nunique(dropna=True)),
        output_file=str(output_file),
    )


def copy_zip_metadata(zip_path: Path, output_root: Path) -> dict[str, Any]:
    copied: dict[str, Any] = {}
    metadata_root = output_root / "_metadata"
    metadata_root.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        for name in ("metadata.json", "manifest.json", "condition.json"):
            if name not in zf.namelist():
                continue
            target = metadata_root / name
            with zf.open(name) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            copied[name] = str(target)
    return copied


def write_manifest(
    output_root: Path,
    zip_path: Path,
    entries: list[tuple[str, dt.date]],
    results: list[EntryResult],
    copied_metadata: dict[str, Any],
) -> Path:
    built = [result for result in results if not result.skipped]
    skipped = [result for result in results if result.skipped]
    manifest = {
        "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_zip": str(zip_path),
        "output_root": str(output_root),
        "partitioning": "hive/date=YYYY-MM-DD with one part-00000.parquet per date",
        "requested_entries": len(entries),
        "built_entries": len(built),
        "skipped_entries": len(skipped),
        "rows_built": sum(result.rows for result in built),
        "min_date": min((date_value for _, date_value in entries), default=None).isoformat()
        if entries
        else None,
        "max_date": max((date_value for _, date_value in entries), default=None).isoformat()
        if entries
        else None,
        "schema": OUTPUT_SCHEMA.names,
        "copied_metadata": copied_metadata,
        "results": [asdict(result) for result in results],
    }
    manifest_path = output_root / "_metadata" / "extract_manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    return manifest_path


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level), format="%(asctime)s %(levelname)s %(message)s")

    zip_path = args.zip_path or newest_equs_zip(Path.home() / "Downloads")
    zip_path = zip_path.expanduser().resolve()
    output_root = args.output_root.resolve()
    start_date = parse_date_arg(args.start_date)
    end_date = parse_date_arg(args.end_date)

    entries = list_dbn_entries(zip_path, start_date, end_date, args.limit)
    if not entries:
        raise RuntimeError(f"No ohlcv-1d DBN entries matched in {zip_path}")

    logging.info("source zip: %s", zip_path)
    logging.info("output root: %s", output_root)
    logging.info("matched %d date files", len(entries))

    copied_metadata = copy_zip_metadata(zip_path, output_root)
    results: list[EntryResult] = []
    for index, (entry_name, date_value) in enumerate(entries, start=1):
        result = extract_entry(
            zip_path=zip_path,
            entry_name=entry_name,
            date_value=date_value,
            output_root=output_root,
            overwrite=args.overwrite,
            compression=args.compression,
        )
        results.append(result)
        status = "skipped" if result.skipped else f"wrote {result.rows} rows"
        logging.info("%d/%d %s %s", index, len(entries), date_value.isoformat(), status)

    manifest_path = write_manifest(output_root, zip_path, entries, results, copied_metadata)
    built_rows = sum(result.rows for result in results if not result.skipped)
    print(
        f"extracted {len([r for r in results if not r.skipped])} date files "
        f"({built_rows} rows built, {len([r for r in results if r.skipped])} skipped) "
        f"to {output_root}"
    )
    print(f"manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
