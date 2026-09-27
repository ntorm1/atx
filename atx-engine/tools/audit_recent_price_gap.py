"""Read-only, hash-pinned source-record forensics; never infer a terminal return.

Only the explicitly requested ID(s) or exact historical ticker(s), within
[start,end), are emitted. Ticker matches never establish security identity.
Unprunable
Parquet groups may physically decode other rows in the eight projected columns.
No QA, duplicate removal, price statistics, universe selection or warehouse I/O.
The deadline is checked between hash chunks/Arrow batches, not an OS hard timeout.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

COLUMNS = ("tradingDate", "securityID", "ticker_tk", "close", "volume",
           "cumulReturnFactor", "returnFactor", "totalReturn")
MAX_FOOTER = 32 << 20
MAX_GROUP_PROJECTED = 128 << 20
MAX_OUTPUT = 16 << 20


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def identity(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


class Deadline:
    def __init__(self, seconds):
        if not math.isfinite(seconds) or not 0 < seconds <= 120:
            raise ValueError("max_seconds must be finite and in (0,120]")
        self.started = time.monotonic()
        self.end = self.started + seconds

    def check(self, stage):
        if time.monotonic() > self.end:
            raise TimeoutError(f"deadline at {stage}; no complete audit published")


def exact_value(array, row):
    scalar = array[row]
    if not scalar.is_valid:
        return None, None
    value = scalar.as_py()
    if pa.types.is_floating(array.type):
        width = array.type.bit_width // 8
        begin = (array.offset + row) * width
        # Arrow numeric buffers are little-endian. Retain original IEEE bits,
        # including signed zero and NaN payloads, instead of JSON NaN literals.
        bits = bytes(memoryview(array.buffers()[1])[begin:begin + width]).hex()
        if not math.isfinite(value):
            value = {"nonfinite": "nan" if math.isnan(value) else ("+inf" if value > 0 else "-inf")}
        return value, bits
    if isinstance(value, dt.date):
        return value.isoformat(), None
    if isinstance(value, str) and len(value.encode("utf8")) > 16384:
        raise ValueError("historical ticker exceeds bounded record size")
    return value, None


def audit(source: Path, output: Path, source_sha256: str, instrument_id: int | None,
          start: str, end: str, max_seconds=120, max_records=10000, batch_rows=65536,
          *, instrument_ids: list[int] | None = None, tickers: list[str] | None = None):
    deadline = Deadline(max_seconds)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", source_sha256):
        raise ValueError("an external SHA256 source pin is required")
    batch_mode = instrument_ids is not None
    ticker_mode = tickers is not None
    if ticker_mode:
        if instrument_id is not None or batch_mode or not 1 <= len(tickers) <= 64:
            raise ValueError("choose IDs or1..64 unique exact historical tickers")
        if any(type(x) is not str or not 1 <= len(x.encode("utf8")) <= 128 or
               any(ord(c) < 32 or ord(c) == 127 for c in x) for x in tickers):
            raise ValueError("tickers must be nonempty bounded strings without control characters")
        requested = sorted(tickers)  # No case folding, stripping or alias conversion.
        if len(set(requested)) != len(requested):
            raise ValueError("historical ticker selectors must be unique")
    elif batch_mode:
        if instrument_id is not None or not 1 <= len(instrument_ids) <= 64:
            raise ValueError("choose single ID or a batch of1..64 unique IDs")
        if any(type(x) is not int or not 0 < x < 1 << 63 for x in instrument_ids):
            raise ValueError("batch IDs must be positive int64 values")
        requested = sorted(instrument_ids)
        if len(set(requested)) != len(requested):
            raise ValueError("batch IDs must be unique")
    else:
        requested = [instrument_id]
    if ((not ticker_mode and any(type(x) is not int or not 0 < x < 1 << 63 for x in requested))
            or not 1 <= max_records <= 10000):
        raise ValueError("invalid positive ID or record budget")
    if not 1024 <= batch_rows <= 65536:
        raise ValueError("batch_rows must be in [1024,65536]")
    first, last = dt.date.fromisoformat(start), dt.date.fromisoformat(end)
    if not 0 < (last - first).days <= 366:
        raise ValueError("explicit ordered date window must be at most366 days")
    if output.exists() or not output.parent.is_dir() or source.resolve() == output.resolve():
        raise ValueError("output must be a new file in an existing directory")

    records, groups, records_bytes = [], [], 0
    counts_by_id = {str(x): 0 for x in requested} if not ticker_mode else {}
    counts_by_ticker = {x: 0 for x in requested} if ticker_mode else {}
    id_set = pa.array(requested, type=pa.int64()) if batch_mode else None
    ticker_set = pa.array(requested, type=pa.string()) if ticker_mode else None
    with source.open("rb") as handle:
        captured = os.fstat(handle.fileno())
        if not 12 <= captured.st_size <= 16 << 30:
            raise ValueError("source extent outside bounded audit contract")
        digest = hashlib.sha256()
        while block := handle.read(1 << 20):
            digest.update(block)
            deadline.check("source hash")
        actual_sha = digest.hexdigest()
        if actual_sha != source_sha256.lower():
            raise ValueError("source SHA256 does not match external pin")
        handle.seek(-8, os.SEEK_END)
        trailer = handle.read(8)
        footer_size = int.from_bytes(trailer[:4], "little")
        if trailer[4:] != b"PAR1" or not 0 < footer_size <= min(MAX_FOOTER, captured.st_size - 12):
            raise ValueError("invalid or oversized Parquet footer")
        handle.seek(-(footer_size + 8), os.SEEK_END)
        footer_sha = hashlib.sha256(handle.read(footer_size + 8)).hexdigest()
        handle.seek(0)
        parquet = pq.ParquetFile(handle, thrift_string_size_limit=MAX_FOOTER,
                                 thrift_container_size_limit=1_000_000)
        schema, metadata = parquet.schema_arrow, parquet.metadata
        if metadata.num_row_groups > 4096 or metadata.num_rows > 100_000_000:
            raise ValueError("source geometry exceeds audit budget")
        if schema.field("tradingDate").type != pa.date32() or schema.field("securityID").type != pa.int64():
            raise ValueError("expected date32 tradingDate and int64 securityID")
        if not pa.types.is_string(schema.field("ticker_tk").type):
            raise ValueError("expected string historical ticker_tk")
        for name in COLUMNS[3:]:
            if schema.field(name).type not in (pa.float32(), pa.float64()):
                raise ValueError(f"expected float32/64 source field {name}")
        projected_indices = [metadata.schema.names.index(name) for name in COLUMNS]
        for group_index in range(metadata.num_row_groups):
            deadline.check("row-group admission")
            group = metadata.row_group(group_index)
            reason = None
            id_stats = group.column(projected_indices[1]).statistics
            date_stats = group.column(projected_indices[0]).statistics
            ticker_stats = group.column(projected_indices[2]).statistics if ticker_mode else None
            ticker_range = bool(ticker_stats and ticker_stats.has_min_max and
                                isinstance(ticker_stats.min, str) and isinstance(ticker_stats.max, str))
            if ticker_mode and ticker_range and not any(ticker_stats.min <= x <= ticker_stats.max for x in requested):
                reason = "ticker_tk-footer-range"
            if not ticker_mode and id_stats and id_stats.has_min_max and not any(id_stats.min <= x <= id_stats.max for x in requested):
                reason = "securityID-footer-range"
            if date_stats and date_stats.has_min_max and (date_stats.max < first or date_stats.min >= last):
                reason = "date-footer-range"
            entry = {"row_group": group_index, "source_rows": group.num_rows,
                     "id_stats_available": bool(id_stats and id_stats.has_min_max),
                     "date_stats_available": bool(date_stats and date_stats.has_min_max),
                     "pruned_by": reason, "matched_records": 0}
            if ticker_mode:
                entry["ticker_stats_available"] = ticker_range
            groups.append(entry)
            if reason:
                continue
            projected_bytes = sum(group.column(i).total_uncompressed_size for i in projected_indices)
            if projected_bytes < 0 or projected_bytes > MAX_GROUP_PROJECTED:
                raise ValueError("projected row-group extent exceeds128MiB decode admission")
            entry["projected_uncompressed_bytes"] = projected_bytes
            offset = 0
            for batch in parquet.iter_batches(batch_size=batch_rows, row_groups=[group_index],
                                              columns=list(COLUMNS), use_threads=False):
                deadline.check("projected batch")
                dates, ids = batch.column(0), batch.column(1)
                if ticker_mode:
                    selected_names = pc.is_in(batch.column(2), value_set=ticker_set)
                else:
                    selected_names = pc.is_in(ids, value_set=id_set) if batch_mode else pc.equal(ids, pa.scalar(instrument_id, pa.int64()))
                selected = pc.and_(selected_names,
                                   pc.and_(pc.greater_equal(dates, pa.scalar(first)),
                                           pc.less(dates, pa.scalar(last))))
                indices = pc.indices_nonzero(pc.fill_null(selected, False)).to_pylist()
                if len(records) + len(indices) > max_records:
                    raise ValueError("matching records exceed explicit budget; no truncation")
                for row in indices:
                    record = {"row_group": group_index, "row_in_group": offset + row,
                              "values": {}, "ieee754_le_hex": {}}
                    for column, name in enumerate(COLUMNS):
                        value, bits = exact_value(batch.column(column), row)
                        record["values"][name] = value
                        if bits is not None:
                            record["ieee754_le_hex"][name] = bits
                    records_bytes += len(canonical(record).encode("utf8"))
                    if records_bytes > MAX_OUTPUT // 2:
                        raise ValueError("matching record bytes exceed output budget")
                    records.append(record)
                    if ticker_mode:
                        counts_by_ticker[record["values"]["ticker_tk"]] += 1
                    else:
                        counts_by_id[str(record["values"]["securityID"])] += 1
                entry["matched_records"] += len(indices)
                offset += batch.num_rows
            if offset != group.num_rows:
                raise ValueError("decoded row count differs from admitted footer")
        if identity(captured) != identity(os.fstat(handle.fileno())) or identity(captured) != identity(source.stat()):
            raise ValueError("source changed during captured audit")

    result = {"schema": "atx.recent-price-gap-audit/v1", "status": "complete",
              "source": str(source.resolve()), "source_sha256": actual_sha,
              "source_bytes": captured.st_size, "source_mtime_ns": captured.st_mtime_ns,
              "footer_and_trailer_sha256": footer_sha, "footer_bytes": footer_size,
              "instrument_id": instrument_id, "start_inclusive": start, "end_exclusive": end,
              "source_types": {name: str(schema.field(name).type) for name in COLUMNS},
              "row_groups": groups, "records": records,
              "records_sha256": hashlib.sha256(canonical(records).encode("utf8")).hexdigest(),
              "record_order": "original-row-group-and-row-order;duplicates-retained",
              "scope": "exact-source-records-only;no-QA-repair-or-terminal-return-inference",
              "physical_decode": "unprunable groups decode projected columns before ID/date filtering",
              "deadline": "cooperative-between-hash-chunks-and-Arrow-batches;max120seconds"}
    if ticker_mode:
        result["schema"] = "atx.recent-price-gap-ticker-audit/v1"
        del result["instrument_id"]
        result["historical_tickers"] = requested
        result["record_counts_by_ticker"] = counts_by_ticker
        result["selector_policy"] = "case-sensitive-exact-source-ticker_tk;no-normalization-or-security-identity-inference"
        result["batch_policy"] = "one-source-hash-and-one-projected-pass;max64-unique-tickers;global-record-byte-budgets"
        result["physical_decode"] = "unprunable groups decode projected columns before exact historical ticker/date filtering"
    elif batch_mode:
        result["schema"] = "atx.recent-price-gap-batch-audit/v1"
        del result["instrument_id"]
        result["instrument_ids"] = requested
        result["record_counts_by_id"] = counts_by_id
        result["batch_policy"] = "one-source-hash-and-one-projected-pass;max64-unique-IDs;global-record-byte-budgets"
    payload = (canonical(result) + "\n").encode("utf8")
    if len(payload) > MAX_OUTPUT:
        raise ValueError("audit JSON exceeds16MiB output bound")
    deadline.check("exclusive publication")
    with output.open("xb") as out:
        out.write(payload)
        out.flush()
        os.fsync(out.fileno())
    receipt = {"output": str(output.resolve()), "sha256": hashlib.sha256(payload).hexdigest(),
               "records": len(records), "row_groups_decoded": sum(g["pruned_by"] is None for g in groups),
               "elapsed_seconds": time.monotonic() - deadline.started}
    receipt["record_counts_by_ticker" if ticker_mode else "record_counts_by_id"] = (
        counts_by_ticker if ticker_mode else counts_by_id)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--id", type=int)
    selection.add_argument("--ids", type=int, nargs="+", help="batch of at most64 unique positive IDs")
    selection.add_argument("--tickers", nargs="+", help="at most64 exact, case-sensitive historical ticker_tk values; no identity inference")
    parser.add_argument("--start", required=True, help="inclusive YYYY-MM-DD")
    parser.add_argument("--end", required=True, help="exclusive YYYY-MM-DD")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-seconds", type=float, default=120)
    parser.add_argument("--max-records", type=int, default=10000)
    args = parser.parse_args()
    try:
        print(canonical(audit(args.source, args.out, args.source_sha256, args.id,
                              args.start, args.end, args.max_seconds, args.max_records,
                              instrument_ids=args.ids, tickers=args.tickers)))
    except (ValueError, OSError, TimeoutError, KeyError, pa.ArrowException) as error:
        print(f"audit refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
