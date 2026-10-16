#!/usr/bin/env python3
"""Prepare label-isolated quote cohorts, run a native fitter, and score its output.

The source adapter is intentionally separate from model execution. Only raw
contract/quote/environment fields reach quotes.parquet or quotes.csv. Reference
prices and vols live in labels.parquet and are opened only by preparation/scoring.
"""
from __future__ import annotations

import argparse
from collections import Counter
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
import polars as pl
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq


SCHEMA_VERSION = 1
# Snapshot the executing adapter at import, before a long-running preparation
# can overlap an unrelated source edit in the shared workspace.
ADAPTER_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
# This list is exhaustive: vendor IVs, greeks, synthetic spots, model flags,
# and model fair values cannot be passed to normalize_quotes.
INPUT_COLUMNS = (
    "okey_tk", "okey_dt", "okey_xx", "okey_cp", "date", "timestamp",
    "undSecKey_tk", "uBid", "uAsk", "bidPrc", "askPrc", "bidSz", "askSz",
    "years", "yearsC", "rate", "sdiv", "ddiv", "ddivPv", "exType",
)
LABEL_COLUMNS = ("srPrc", "srVol")
QUOTE_SCHEMA = pa.schema([
    ("row_id", pa.uint64()), ("chain_id", pa.uint64()),
    ("snapshot_ns", pa.int64()), ("observation_ns", pa.int64()),
    ("underlier", pa.string()), ("root", pa.string()),
    ("expiry_days", pa.int32()), ("strike", pa.float64()),
    ("is_call", pa.uint8()), ("exercise", pa.string()),
    *[(name, pa.float64()) for name in ("spot_bid", "spot_ask", "spot", "bid", "ask")],
    ("bid_size", pa.int32()), ("ask_size", pa.int32()),
    *[(name, pa.float64()) for name in
      ("years", "years_calendar", "rate", "sdiv", "ddiv", "ddiv_pv")],
    ("split", pa.string()), ("input_status", pa.string()),
])
LABEL_SCHEMA = pa.schema([
    ("row_id", pa.uint64()), ("reference_price", pa.float64()),
    ("reference_vol", pa.float64()),
])
PREDICTION_COLUMNS = ("row_id", "predicted_price", "predicted_vol", "status", "chain_id")
POLICY_METADATA_COLUMNS = ("resolved_profile", "policy_source", "configured_curve", "actual_curve")
POLICY_METADATA_STATES = {
    "reported": "nonempty metadata supplied by this prediction artifact",
    "value_absent": "metadata column exists but this prediction row has an empty/null value",
    "column_absent": "prediction artifact predates or omits this optional metadata column",
    "prediction_absent": "the selected quote row has no row in the prediction artifact",
}
DIVIDEND_CONVENTIONS = {
    "funding-pv": "aggregate_funding_pv_approximation",
    "escrowed-pv": "aggregate_pv_approximation",
    "none": "continuous_yield",
}
RAW_STRATA = ("contract_kind", "moneyness_band", "cash_dividends", "liquid_core")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stable_id(parts: tuple | list) -> int:
    payload = json.dumps(parts, ensure_ascii=True, separators=(",", ":")).encode()
    return int.from_bytes(hashlib.blake2b(payload, digest_size=8,
                                        person=b"atx-quotes-v1").digest(), "big")


def split_for(underlier: str, fraction: float = 0.2, seed: str = "atx-vol-v1") -> str:
    # Entire underliers stay together, including every root, expiry and time.
    return ("validation" if stable_id([seed, underlier]) < int(fraction * (1 << 64))
            else "development")


def write_json(path: Path, data: dict, *, overwrite: bool = True, durable: bool = False) -> None:
    payload = json.dumps(data, indent=2, sort_keys=True, allow_nan=False) + "\n"
    with path.open("w" if overwrite else "x", encoding="utf-8") as stream:
        stream.write(payload)
        if durable:
            stream.flush()
            os.fsync(stream.fileno())


def normalize_quotes(batch: pa.RecordBatch, offset: int, *, bucket_minutes: int,
                     validation_fraction: float, split_seed: str) -> pl.DataFrame:
    """Pure model-input conversion; reject any reference/unknown column at entry."""
    if set(batch.schema.names) != set(INPUT_COLUMNS):
        raise ValueError("normalize_quotes requires exactly the raw-input allowlist")
    data = pl.from_arrow(batch)
    ids = pl.Series("row_id", np.arange(offset, offset + batch.num_rows, dtype=np.uint64))
    data = data.with_columns(ids)
    # Explicit timestamp cast supports source timestamp[us, UTC] and ns sources.
    snap = pl.col("date").cast(pl.Datetime("ns", "UTC"))
    norm = data.select(
        "row_id",
        snap.cast(pl.Int64).alias("snapshot_ns"),
        pl.col("timestamp").cast(pl.Datetime("ns", "UTC")).cast(pl.Int64)
          .alias("observation_ns"),
        pl.col("undSecKey_tk").cast(pl.String).alias("underlier"),
        pl.col("okey_tk").cast(pl.String).alias("root"),
        pl.col("okey_dt").cast(pl.Date).cast(pl.Int32).alias("expiry_days"),
        pl.col("okey_xx").cast(pl.Float64).alias("strike"),
        pl.when(pl.col("okey_cp").str.to_lowercase().is_in(["call", "c"]))
          .then(pl.lit(1, pl.UInt8))
          .when(pl.col("okey_cp").str.to_lowercase().is_in(["put", "p"]))
          .then(pl.lit(0, pl.UInt8)).otherwise(pl.lit(255, pl.UInt8)).alias("is_call"),
        pl.when(pl.col("exType").cast(pl.String).str.to_lowercase() == "american")
          .then(pl.lit("American")).otherwise(pl.col("exType").cast(pl.String))
          .alias("exercise"),
        *[pl.col(src).cast(pl.Float64).alias(dest) for src, dest in (
            ("uBid", "spot_bid"), ("uAsk", "spot_ask"), ("bidPrc", "bid"),
            ("askPrc", "ask"), ("years", "years"), ("yearsC", "years_calendar"),
            ("rate", "rate"), ("sdiv", "sdiv"), ("ddiv", "ddiv"), ("ddivPv", "ddiv_pv"))],
        pl.col("bidSz").cast(pl.Int32).alias("bid_size"),
        pl.col("askSz").cast(pl.Int32).alias("ask_size"),
    ).with_columns(((pl.col("spot_bid") + pl.col("spot_ask")) * 0.5).alias("spot"))
    # Compute hashes once per unique chain/underlier in this batch, not per row.
    keys = ["underlier", "root", "snapshot_ns", "expiry_days"]
    groups = norm.select(keys).unique(maintain_order=True)
    groups = groups.with_columns(pl.Series(
        "chain_id", [stable_id(list(row)) for row in groups.iter_rows()], dtype=pl.UInt64))
    names = norm.select("underlier").unique(maintain_order=True)
    names = names.with_columns(pl.Series(
        "split", [split_for(name or "", validation_fraction, split_seed)
                  for name in names["underlier"]], dtype=pl.String))
    norm = norm.join(groups, on=keys, how="left", nulls_equal=True,
                     maintain_order="left").join(names, on="underlier", how="left",
                                                  nulls_equal=True, maintain_order="left")
    def valid(name: str) -> pl.Expr:
        return pl.col(name).is_not_null() & pl.col(name).is_finite()
    # Admission uses no labels, IVs or vendor quality/model flags. Keep rejected
    # rows in the cohort so a fitter cannot improve metrics by silently omitting.
    status = (
        pl.when(pl.col("exercise").str.to_lowercase().ne("american").fill_null(True))
          .then(pl.lit("unsupported_exercise"))
        .when(pl.col("underlier").is_null() | pl.col("root").is_null()
              | (pl.col("underlier").str.len_chars() == 0)
              | (pl.col("root").str.len_chars() == 0)
              | pl.col("expiry_days").is_null() | pl.col("snapshot_ns").is_null()
              | (pl.col("is_call") > 1) | ~valid("strike") | (pl.col("strike") <= 0))
          .then(pl.lit("invalid_contract"))
        .when(~valid("spot_bid") | ~valid("spot_ask") | (pl.col("spot_bid") <= 0)
              | (pl.col("spot_ask") < pl.col("spot_bid")))
          .then(pl.lit("invalid_underlier_quote"))
        .when(~valid("years") | ~valid("years_calendar") | (pl.col("years") <= 0)
              | (pl.col("years_calendar") <= 0))
          .then(pl.lit("invalid_time"))
        .when(~valid("rate") | ~valid("sdiv") | ~valid("ddiv") | ~valid("ddiv_pv")
              | (pl.col("rate") <= -99) | (pl.col("sdiv") <= -99)
              | (pl.col("ddiv") < 0) | (pl.col("ddiv_pv") < 0))
          .then(pl.lit("invalid_carry"))
        .when(~valid("bid") | ~valid("ask") | (pl.col("bid") < 0)
              | (pl.col("ask") <= 0) | (pl.col("ask") < pl.col("bid")))
          .then(pl.lit("invalid_option_quote"))
        .otherwise(pl.lit("ok")).alias("input_status")
    )
    return norm.with_columns(status).select(QUOTE_SCHEMA.names)


def choose_symbols(source: pq.ParquetFile, args: argparse.Namespace) -> set[str] | None:
    symbols = set(filter(None, (s.strip() for s in args.symbols.split(",")))) or None
    if args.max_underliers is None:
        return symbols
    names: set[str] = set()
    for batch in source.iter_batches(columns=["undSecKey_tk"], batch_size=args.batch_size):
        names.update(v for v in batch.column(0).unique().to_pylist() if v)
    if symbols is not None:
        names &= symbols
    if args.split != "all":
        names = {n for n in names
                 if split_for(n, args.validation_fraction, args.split_seed) == args.split}
    # Hash order avoids an alphabetical large-cap/symbol-prefix selection bias.
    return set(sorted(names, key=lambda n: (stable_id([args.split_seed, n]), n))
               [:args.max_underliers])


def prepare(args: argparse.Namespace) -> dict:
    if not 0 < args.validation_fraction < 1:
        raise ValueError("validation fraction must be strictly between 0 and 1")
    if args.batch_size < 1 or args.bucket_minutes < 1 or 60 % args.bucket_minutes:
        raise ValueError("batch size must be positive; bucket minutes must divide 60")
    if args.max_underliers is not None and args.max_underliers < 1:
        raise ValueError("max underliers must be positive")
    buckets = set(filter(None, args.buckets.split(",")))
    for token in buckets:
        dt.datetime.strptime(token, "%H%M")
        if len(token) != 4:
            raise ValueError("buckets use four-digit New York HHMM")
    source_path = args.source.resolve()
    source = pq.ParquetFile(source_path)
    missing = set(INPUT_COLUMNS + LABEL_COLUMNS) - set(source.schema_arrow.names)
    if missing:
        raise ValueError(f"source missing columns: {sorted(missing)}")
    source_stat = source_path.stat()
    started = time.perf_counter()
    source_hash = sha256(source_path)
    symbols = choose_symbols(source, args)
    args.output.mkdir(parents=True, exist_ok=False)
    quote_path, label_path = args.output / "quotes.parquet", args.output / "labels.parquet"
    quote_writer = pq.ParquetWriter(quote_path, QUOTE_SCHEMA, compression="zstd")
    label_writer = pq.ParquetWriter(label_path, LABEL_SCHEMA, compression="zstd")
    csv_writer = (pacsv.CSVWriter(args.output / "quotes.csv", QUOTE_SCHEMA) if args.csv else None)
    counts: Counter = Counter()
    split_counts: Counter = Counter()
    selected_names: set[str] = set()
    offset = 0
    try:
        # Separate readers are deliberate: normalize_quotes never sees labels.
        quotes = source.iter_batches(columns=list(INPUT_COLUMNS), batch_size=args.batch_size)
        labels = source.iter_batches(columns=list(LABEL_COLUMNS), batch_size=args.batch_size)
        for raw, ref in zip(quotes, labels, strict=True):
            if raw.num_rows != ref.num_rows:
                raise ValueError("input and label reader batch alignment failed")
            norm = normalize_quotes(raw, offset, bucket_minutes=args.bucket_minutes,
                                    validation_fraction=args.validation_fraction,
                                    split_seed=args.split_seed)
            offset += raw.num_rows
            if symbols is not None:
                norm = norm.filter(pl.col("underlier").is_in(sorted(symbols)))
            if args.split != "all":
                norm = norm.filter(pl.col("split") == args.split)
            if buckets:
                norm = norm.filter(pl.col("snapshot_ns").cast(pl.Datetime("ns", "UTC"))
                                   .dt.truncate(f"{args.bucket_minutes}m")
                                   .dt.convert_time_zone("America/New_York")
                                   .dt.strftime("%H%M").is_in(sorted(buckets)))
            if norm.is_empty():
                continue
            selected_names.update(norm["underlier"].drop_nulls().unique().to_list())
            counts.update(norm["input_status"].to_list())
            split_counts.update(norm["split"].to_list())
            quote_table = norm.to_arrow().cast(QUOTE_SCHEMA)
            quote_writer.write_table(quote_table)
            if csv_writer is not None:
                csv_writer.write_table(quote_table)
            index = pa.array((norm["row_id"].to_numpy() - (offset - raw.num_rows)).astype(np.int64))
            label_table = pa.Table.from_arrays([
                quote_table["row_id"],
                ref.column("srPrc").take(index).cast(pa.float64()),
                ref.column("srVol").take(index).cast(pa.float64()),
            ], schema=LABEL_SCHEMA)
            label_writer.write_table(label_table)
    finally:
        quote_writer.close()
        label_writer.close()
        if csv_writer is not None:
            csv_writer.close()
    current_stat = source_path.stat()
    if (source_stat.st_size, source_stat.st_mtime_ns) != (current_stat.st_size, current_stat.st_mtime_ns):
        raise ValueError("source changed during preparation; cohort is incomplete")
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "source": {"path": str(source_path), "sha256": source_hash,
                   "bytes": source_stat.st_size, "mtime_ns": source_stat.st_mtime_ns,
                   "rows": source.metadata.num_rows},
        "adapter_sha256": ADAPTER_SHA256,
        "input_allowlist": list(INPUT_COLUMNS), "label_columns": list(LABEL_COLUMNS),
        "quote_columns": QUOTE_SCHEMA.names,
        "selection": {"split": args.split, "split_seed": args.split_seed,
                      "validation_fraction": args.validation_fraction,
                      "requested_symbols": sorted(filter(None, args.symbols.split(","))),
                      "selected_underliers": sorted(selected_names),
                      "max_underliers": args.max_underliers,
                      "bucket_minutes": args.bucket_minutes,
                      "buckets_new_york": sorted(buckets)},
        "rows_selected": sum(counts.values()), "rows_by_input_status": dict(counts),
        "rows_by_split": dict(split_counts),
        "files": {p.name: {"sha256": sha256(p), "bytes": p.stat().st_size}
                  for p in (quote_path, label_path, args.output / "quotes.csv") if p.exists()},
        "preparation_seconds": time.perf_counter() - started,
        "time_convention": "years is variance time; years_calendar is carry/discount time",
        "dividend_convention": "aggregate cash amount/PV retained; event timing is unavailable",
        "validation_policy": "validation labels must not select model hyperparameters",
    }
    write_json(args.output / "manifest.json", manifest)
    print(json.dumps({"dataset": str(args.output), "rows": manifest["rows_selected"],
                      "input_status": dict(counts), "seconds": manifest["preparation_seconds"]}))
    return manifest


def verify_dataset(dataset: Path, *, labels: bool = False) -> dict:
    manifest = json.loads((dataset / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported dataset schema version")
    if manifest.get("quote_columns") != QUOTE_SCHEMA.names:
        raise ValueError("dataset input schema is not the fixed model-input allowlist")
    required = ["quotes.parquet"] + (["labels.parquet"] if labels else [])
    for name in required + (["quotes.csv"] if "quotes.csv" in manifest["files"] else []):
        if name not in manifest["files"] or sha256(dataset / name) != manifest["files"][name]["sha256"]:
            raise ValueError(f"dataset hash mismatch: {name}")
    if pq.read_schema(dataset / "quotes.parquet") != QUOTE_SCHEMA:
        raise ValueError("quote parquet schema mismatch")
    if labels and pq.read_schema(dataset / "labels.parquet") != LABEL_SCHEMA:
        raise ValueError("label parquet schema mismatch")
    return manifest


def run_native(args: argparse.Namespace) -> dict:
    if args.model == "common-markov" and (args.policy is not None or args.curve is not None):
        raise ValueError("common-markov requires --common-policy; --policy/--curve are incompatible")
    if args.model == "quote-slice" and args.common_policy is not None:
        raise ValueError("--common-policy requires --model common-markov")
    if args.progress is not None and args.model != "common-markov":
        raise ValueError("--progress requires --model common-markov")
    manifest = verify_dataset(args.dataset)
    if "quotes.csv" not in manifest["files"]:
        raise ValueError("native runner needs quotes.csv; prepare with --csv")
    binary = args.binary.resolve()
    binary_hash = sha256(binary)
    output = args.output.resolve()
    receipt_path = Path(str(output) + ".run.json")
    start_path = Path(str(output) + ".start.json")
    if output.exists() or receipt_path.exists() or start_path.exists():
        raise ValueError("prediction/receipt output already exists; use a new output path")
    output.parent.mkdir(parents=True, exist_ok=True)
    extras = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    if any(arg.split("=", 1)[0] in {"--input", "--output", "--curve", "--policy", "--dividends",
                                   "--model", "--common-policy", "--progress"}
           for arg in extras):
        raise ValueError("extra arguments cannot replace provenance-bound "
                         "input/output/model/curve/policy/common-policy/dividends/progress")
    policy_path = None
    policy_hash = None
    policy_document = None
    policy = ((args.common_policy or "auto") if args.model == "common-markov"
              else args.policy or (None if args.curve else "auto"))
    if policy and policy != "auto":
        policy_path = Path(policy).resolve()
        # Keep configuration provenance bounded; the native loader owns schema validation.
        if policy_path.stat().st_size > 1048576:
            raise ValueError("policy configuration exceeds 1 MiB")
        policy_bytes = policy_path.read_bytes()
        policy_hash = hashlib.sha256(policy_bytes).hexdigest()
        policy_document = json.loads(policy_bytes)
        policy = str(policy_path)
    progress_path = args.progress.resolve() if args.progress is not None else None
    protected = {output, receipt_path.resolve(), start_path.resolve(), binary, policy_path} | {
        (args.dataset / name).resolve()
        for name in ("manifest.json", "quotes.parquet", "quotes.csv", "labels.parquet")
    }
    if progress_path is not None:
        if progress_path.exists() or progress_path in protected:
            raise ValueError("progress journal must use a new path distinct from protected artifacts")
        protected.add(progress_path)
    report_path = None
    for index, argument in enumerate(extras):
        if argument.split("=", 1)[0] != "--report":
            continue
        if report_path is not None:
            raise ValueError("native timing report may be specified only once")
        if "=" in argument:
            value = argument.split("=", 1)[1]
        elif index + 1 < len(extras):
            value = extras[index + 1]
        else:
            raise ValueError("native timing report requires an output path")
        if not value or value.startswith("--"):
            raise ValueError("native timing report requires an output path")
        report_path = Path(value).resolve()
        if report_path.exists() or report_path in protected:
            raise ValueError("native timing report must use a new path distinct from predictions/receipt")
    selection = (["--common-policy", policy] if args.model == "common-markov"
                 else ["--curve", args.curve] if args.curve else ["--policy", policy])
    command = [str(binary), "--input", str((args.dataset / "quotes.csv").resolve()),
               "--output", str(output), "--model", args.model, *selection,
               "--dividends", args.dividends,
               *(["--progress", str(progress_path)] if progress_path is not None else []), *extras]
    start_receipt = {
        "schema_version": SCHEMA_VERSION, "state": "started_incomplete",
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "command": command,
        "runner_sha256": ADAPTER_SHA256, "binary": str(binary), "binary_sha256": binary_hash,
        "dataset": str(args.dataset.resolve()),
        "dataset_manifest_sha256": sha256(args.dataset / "manifest.json"),
        "quote_parquet_sha256": manifest["files"]["quotes.parquet"]["sha256"],
        "quote_csv_sha256": manifest["files"]["quotes.csv"]["sha256"],
        "input_rows": manifest["rows_selected"],
        "model": args.model, "curve": args.curve, "policy_selection": policy,
        "policy_sha256": policy_hash, "policy_document": policy_document,
        "dividends": args.dividends, "dividend_model": DIVIDEND_CONVENTIONS[args.dividends],
        "predictions": str(output), "run_receipt": str(receipt_path),
        "native_report": str(report_path) if report_path else None,
        "progress": str(progress_path) if progress_path else None,
        "completion_semantics": "immutable start evidence; only final .run.json certifies "
            "successful artifact publication; progress completion covers numerical processing only",
    }
    write_json(start_path, start_receipt, overwrite=False, durable=True)
    start_hash = sha256(start_path)
    started = time.perf_counter()
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE, text=True)
    elapsed = time.perf_counter() - started
    if result.returncode:
        raise RuntimeError(f"native runner failed with exit code {result.returncode}")
    if sha256(binary) != binary_hash:
        raise ValueError("native binary changed during execution")
    if policy_path is not None and sha256(policy_path) != policy_hash:
        raise ValueError("policy configuration changed during execution")
    native_report = json.loads(result.stdout)
    if not isinstance(native_report, dict):
        raise ValueError("native runner did not return a JSON report object")
    # Ensure model execution did not mutate its declared quote input.
    verify_dataset(args.dataset)
    if sha256(start_path) != start_hash:
        raise ValueError("start receipt changed during execution")
    progress = (_verify_native_progress(progress_path, native_report, start_receipt)
                if progress_path is not None else None)
    receipt = {
        "schema_version": SCHEMA_VERSION, "command": command,
        "binary": str(binary), "binary_sha256": binary_hash,
        "dataset": str(args.dataset.resolve()),
        "quote_parquet_sha256": manifest["files"]["quotes.parquet"]["sha256"],
        "quote_csv_sha256": manifest["files"]["quotes.csv"]["sha256"],
        "predictions": str(output), "prediction_sha256": sha256(output),
        "wall_seconds": elapsed, "model": args.model, "curve": args.curve,
        "policy": policy if args.model == "quote-slice" else None,
        "common_policy": policy if args.model == "common-markov" else None,
        "dividends": args.dividends,
        "dividend_model": DIVIDEND_CONVENTIONS[args.dividends],
        "policy_sha256": policy_hash, "policy_document": policy_document,
        "native_report": native_report,
        "start_receipt": str(start_path), "start_receipt_sha256": start_hash,
        "runner_sha256": ADAPTER_SHA256, "progress": progress,
    }
    write_json(receipt_path, receipt, overwrite=False, durable=True)
    print(json.dumps({"run_receipt": str(receipt_path), "wall_seconds": elapsed}))
    return receipt


def _verify_native_progress(path: Path, report: dict, start: dict) -> dict:
    """Stream the bounded completed journal; partial journals are not run receipts."""
    digest = hashlib.sha256()
    header = None
    completion = None
    snapshots = rows = sequence = 0
    with path.open("rb") as stream:
        while line := stream.readline(4 * 1024 * 1024 + 1):
            if len(line) > 4 * 1024 * 1024 or not line.endswith(b"\n"):
                raise ValueError("progress journal contains oversized or partial record")
            digest.update(line)
            record = json.loads(line)
            if (not isinstance(record, dict) or type(record.get("schema_version")) is not int
                    or record["schema_version"] != 1):
                raise ValueError("progress journal has an invalid record schema")
            if (completion is not None or type(record.get("sequence")) is not int
                    or record["sequence"] != sequence):
                raise ValueError("progress journal has an invalid sequence")
            if sequence == 0:
                header = record
                binding = header.get("run_binding")
                if not isinstance(binding, dict) or not isinstance(binding.get("source"), dict):
                    raise ValueError("progress journal has an invalid provenance object")
                source = binding["source"]
                if (header.get("record_type") != "start"
                        or source.get("input_sha256") != start["quote_csv_sha256"]
                        or source.get("policy_sha256") != start["policy_sha256"]
                        or source.get("executable_sha256") != start["binary_sha256"]
                        or binding.get("model") != start["model"]
                        or binding.get("dividends") != start["dividends"]
                        or report.get("model") != start["model"]
                        or report.get("dividend_model") != start["dividends"]
                        or header.get("configuration") != report.get("policy_definition")
                        or not isinstance(binding.get("configuration_sha256"), str)
                        or binding["configuration_sha256"] != report.get("progress", {}).get(
                            "configuration_sha256")
                        or header.get("input_rows") != start["input_rows"]
                        or header.get("run_binding_sha256") != report.get("progress", {}).get(
                            "run_binding_sha256")):
                    raise ValueError("progress journal provenance disagrees with run")
            elif record.get("run_binding_sha256") != header["run_binding_sha256"]:
                raise ValueError("progress journal run binding changed")
            elif record.get("record_type") == "snapshot":
                snapshots += 1
                count = record.get("rows")
                if type(count) is not int or count <= 0:
                    raise ValueError("progress journal has an invalid row count")
                rows += count
                if (record.get("completed_snapshots") != snapshots
                        or record.get("completed_rows") != rows):
                    raise ValueError("progress journal cumulative counts disagree")
            elif record.get("record_type") == "complete":
                completion = record
            else:
                raise ValueError("progress journal has an invalid record type")
            sequence += 1
            if sequence > start["input_rows"] + 2:
                raise ValueError("progress journal exceeds record limit")
    if (completion is None or header is None
            or completion.get("state") != "numerical_processing_complete"
            or completion.get("completed_snapshots") != snapshots
            or snapshots != header.get("snapshots_expected") or snapshots != report.get("snapshots")
            or completion.get("completed_rows") != rows
            or rows != report.get("progress", {}).get("completed_rows")
            or rows > start["input_rows"]
            or header.get("rows_excluded_by_input_status") != start["input_rows"] - rows
            or completion.get("rows_excluded_by_input_status") != start["input_rows"] - rows
            or completion.get("input_rows") != report.get("rows")
            or report.get("rows") != start["input_rows"]):
        raise ValueError("progress journal incomplete or completion counts disagree")
    return {"path": str(path), "sha256": digest.hexdigest(), "records": sequence,
            "completed_snapshots": snapshots, "completed_rows": rows,
            "state": "numerical_processing_complete"}


def scan_predictions(path: Path) -> pl.LazyFrame:
    """Retain optional policy provenance without inventing it for older artifacts."""
    if path.suffix.lower() == ".parquet":
        frame = pl.scan_parquet(path)
    else:
        frame = pl.scan_csv(path, schema_overrides={"row_id": pl.UInt64, "chain_id": pl.UInt64,
                              "predicted_price": pl.Float64, "predicted_vol": pl.Float64,
                              "status": pl.String,
                              **dict.fromkeys(POLICY_METADATA_COLUMNS, pl.String)},
                            null_values="")
    # Float NaNs are parsed as floats and excluded by the finite-value masks.
    # Global null tokens such as "nan" would corrupt a legal profile named nan.
    schema = frame.collect_schema()
    missing = set(PREDICTION_COLUMNS) - set(schema.names())
    if missing:
        raise ValueError(f"predictions missing columns: {sorted(missing)}")
    columns = [pl.col(name) for name in PREDICTION_COLUMNS]
    invalid_metadata = []
    for name in POLICY_METADATA_COLUMNS:
        state_name = f"{name}_state"
        if name not in schema:
            columns.extend([pl.lit(None, dtype=pl.String).alias(name),
                            pl.lit("column_absent").alias(state_name)])
            continue
        if schema[name] not in (pl.String, pl.Null):
            raise ValueError(f"prediction metadata {name} must have string or null type")
        value = pl.col(name).cast(pl.String).replace("", None)
        columns.extend([value.alias(name),
                        pl.when(value.is_null()).then(pl.lit("value_absent"))
                          .otherwise(pl.lit("reported")).alias(state_name)])
        invalid_metadata.append(((value.str.len_chars() > 128)
                                  | value.str.contains(r"[\x00-\x1f\x7f]"))
                                 .fill_null(False).any().alias(name))
    if invalid_metadata:
        invalid = frame.select(invalid_metadata).collect().to_dicts()[0]
        if any(invalid.values()):
            names = [name for name, rejected in invalid.items() if rejected]
            raise ValueError(f"prediction metadata contains invalid identifiers: {names}")
    return frame.select(columns)


def _error_metrics(error: pl.Expr, prefix: str) -> list[pl.Expr]:
    return [error.mean().alias(f"{prefix}_bias"), error.abs().mean().alias(f"{prefix}_mae"),
            error.pow(2).mean().sqrt().alias(f"{prefix}_rmse"),
            error.abs().median().alias(f"{prefix}_median_abs"),
            error.abs().quantile(.95).alias(f"{prefix}_p95_abs"),
            error.abs().quantile(.99).alias(f"{prefix}_p99_abs")]


def _metrics() -> list[pl.Expr]:
    result = [pl.len().alias("rows"), pl.col("eligible").sum().alias("eligible_rows"),
              pl.col("predicted").sum().alias("predicted_rows"),
              pl.col("price_prediction_valid").sum().alias("price_prediction_rows"),
              pl.col("vol_prediction_valid").sum().alias("vol_prediction_rows"),
              pl.col("price_reference_valid").sum().alias("price_label_rows"),
              pl.col("vol_reference_valid").sum().alias("vol_label_rows"),
              pl.col("price_valid").sum().alias("price_scored_rows"),
              pl.col("vol_valid").sum().alias("vol_scored_rows")]
    for col, unit in (("price_error", "price"), ("vol_error", "vol_bp"),
                      ("spread_error", "price_in_spreads"),
                      ("midpoint_error", "midpoint_price")):
        result.extend(_error_metrics(pl.col(col), unit))
    result.extend([pl.col("inside_quotes").mean().alias("price_quote_containment"),
                   (pl.col("reference_inside_quotes") == 0).sum().alias("reference_outside_quote_rows"),
                   pl.col("reference_inside_quotes").mean().alias("reference_quote_containment")])
    return result


def _load_evaluation(dataset: Path, receipt_path: Path, split: str,
                     tick_size: float) -> tuple[dict, dict, pl.LazyFrame]:
    """Validate provenance/identities and retain the full selected quote cohort."""
    if not np.isfinite(tick_size) or tick_size <= 0:
        raise ValueError("tick size must be finite and positive")
    manifest = verify_dataset(dataset, labels=True)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    predictions = Path(receipt["predictions"])
    if receipt["quote_parquet_sha256"] != manifest["files"]["quotes.parquet"]["sha256"]:
        raise ValueError("run receipt belongs to a different quote cohort")
    if ("quotes.csv" in manifest["files"]
            and receipt.get("quote_csv_sha256") != manifest["files"]["quotes.csv"]["sha256"]):
        raise ValueError("run receipt quote CSV digest disagrees with the native input")
    if receipt["prediction_sha256"] != sha256(predictions):
        raise ValueError("prediction hash mismatch")
    quotes = pl.scan_parquet(dataset / "quotes.parquet")
    labels = pl.scan_parquet(dataset / "labels.parquet")
    pred = scan_predictions(predictions)
    # One-to-one joins are necessary: duplicate predictions must never weight a
    # convenient contract twice, and omitted contracts remain coverage failures.
    for name, frame in (("quotes", quotes), ("labels", labels), ("predictions", pred)):
        c = frame.select(pl.len().alias("n"), pl.col("row_id").n_unique().alias("unique"),
                         pl.col("row_id").null_count().alias("nulls")).collect().row(0)
        if c[0] != c[1] or c[2]:
            raise ValueError(f"{name} contains duplicate or null row identities")
    if pred.select(pl.col("chain_id").null_count()).collect().item():
        raise ValueError("predictions contain null chain identities")
    for name, frame in (("labels", labels), ("predictions", pred)):
        extra = frame.join(quotes.select("row_id"), on="row_id", how="anti").select(pl.len()).collect().item()
        if extra:
            raise ValueError(f"{name} contains {extra} unknown row identities")
    if labels.select(pl.len()).collect().item() != manifest["rows_selected"]:
        raise ValueError("labels do not cover the complete selected quote cohort")
    joined = quotes.join(labels, on="row_id", how="left", validate="1:1").join(
        pred.rename({"chain_id": "prediction_chain_id"}), on="row_id", how="left", validate="1:1")
    mismatch = joined.filter(pl.col("prediction_chain_id").is_not_null()
                             & (pl.col("chain_id") != pl.col("prediction_chain_id")))
    if mismatch.select(pl.len()).collect().item():
        raise ValueError("prediction chain identity disagrees with its quote row")
    if split != "all":
        joined = joined.filter(pl.col("split") == split)
    # A missing prediction differs from an old-format or unresolved prediction.
    # The states are separate keys, so a real profile can never collide with a
    # synthetic string used as a missing-value group label.
    joined = joined.with_columns([
        pl.col(f"{name}_state").fill_null("prediction_absent")
        for name in POLICY_METADATA_COLUMNS
    ])
    def good(col: str) -> pl.Expr:
        return pl.col(col).is_not_null() & pl.col(col).is_finite()
    joined = joined.with_columns(
        (pl.col("input_status") == "ok").alias("eligible"),
        (pl.col("status") == "ok").fill_null(False).alias("predicted"),
    ).with_columns(
        (pl.col("eligible") & pl.col("predicted") & good("predicted_price")
         & (pl.col("predicted_price") >= 0)).fill_null(False).alias("price_prediction_valid"),
        (pl.col("eligible") & pl.col("predicted") & good("predicted_vol")
         & (pl.col("predicted_vol") > 0)).fill_null(False).alias("vol_prediction_valid"),
        (pl.col("eligible") & good("reference_price") & (pl.col("reference_price") >= 0))
          .fill_null(False).alias("price_reference_valid"),
        (pl.col("eligible") & good("reference_vol") & (pl.col("reference_vol") > 0))
          .fill_null(False).alias("vol_reference_valid"),
    ).with_columns(
        (pl.col("price_prediction_valid") & pl.col("price_reference_valid")).alias("price_valid"),
        (pl.col("vol_prediction_valid") & pl.col("vol_reference_valid")).alias("vol_valid"),
    ).with_columns(
        pl.when(pl.col("price_valid")).then(pl.col("predicted_price") - pl.col("reference_price"))
          .otherwise(None).alias("price_error"),
        pl.when(pl.col("vol_valid")).then((pl.col("predicted_vol") - pl.col("reference_vol")) * 1e4)
          .otherwise(None).alias("vol_error"),
        pl.when(pl.col("price_prediction_valid")).then(pl.col("predicted_price").is_between(pl.col("bid"), pl.col("ask")))
          .otherwise(None).cast(pl.Float64).alias("inside_quotes"),
        pl.when(pl.col("price_reference_valid"))
          .then(pl.col("reference_price").is_between(pl.col("bid"), pl.col("ask")))
          .otherwise(None).cast(pl.Float64).alias("reference_inside_quotes"),
        # Same population as the fit error: this reveals whether smoothing
        # improved reference parity versus the unsmoothed quote midpoint.
        pl.when(pl.col("price_valid"))
          .then((pl.col("bid") + pl.col("ask")) * .5 - pl.col("reference_price"))
          .otherwise(None).alias("midpoint_error"),
        (pl.col("years_calendar") * 365.25).alias("dte"),
        (pl.col("strike") / pl.col("spot")).log().alias("log_moneyness"),
    ).with_columns(
        (pl.col("price_error") / (pl.col("ask") - pl.col("bid")).clip(lower_bound=tick_size))
          .alias("spread_error"),
        pl.col("dte").cut([1., 7., 30., 90., 365.],
                           labels=["<=1", "1-7", "7-30", "30-90", "90-365", ">365"])
          .cast(pl.String).alias("dte_band"),
    )
    return manifest, receipt, _add_reporting_strata(joined)


def _add_reporting_strata(frame: pl.LazyFrame) -> pl.LazyFrame:
    """Raw-input partitions are reporting only and never change eligibility."""
    abs_k = pl.col("log_moneyness").abs()
    mid = (pl.col("bid") + pl.col("ask")) * .5
    raw_core = ((abs_k <= .2) & (pl.col("bid") > 0)
                & ((pl.col("ask") - pl.col("bid")) / mid <= .5))
    return frame.with_columns(
        pl.when(pl.col("root").is_null() | (pl.col("root").str.len_chars() == 0))
          .then(pl.lit("unknown"))
          .when(pl.col("root").str.contains(r"[1-9]$"))
          .then(pl.lit("adjusted_suffix"))
          .otherwise(pl.lit("standard_symbol")).alias("contract_kind"),
        pl.when(abs_k.is_null() | ~abs_k.is_finite()).then(pl.lit("unknown"))
          .when(abs_k <= .2).then(pl.lit("core_abs_log_le_0.2"))
          .when(abs_k <= .7).then(pl.lit("wing_abs_log_0.2_0.7"))
          .otherwise(pl.lit("far_wing_abs_log_gt_0.7")).alias("moneyness_band"),
        pl.when(pl.col("ddiv_pv").is_null() | ~pl.col("ddiv_pv").is_finite()
                | (pl.col("ddiv_pv") < 0)).then(pl.lit("unknown"))
          .when(pl.col("ddiv_pv") > 0).then(pl.lit("nonzero"))
          .otherwise(pl.lit("zero")).alias("cash_dividends"),
        raw_core.fill_null(False).alias("liquid_core"),
        # This is a LABEL diagnostic, deliberately separate from RAW_STRATA.
        pl.when(pl.col("reference_inside_quotes") == 1).then(pl.lit("inside"))
          .when(pl.col("reference_inside_quotes") == 0).then(pl.lit("outside"))
          .otherwise(pl.lit("ineligible_or_missing")).alias("reference_location"),
    )


def _with_coverage(metrics: dict) -> dict:
    eligible = metrics["eligible_rows"]
    for kind in ("price", "vol"):
        metrics[f"{kind}_coverage"] = metrics[f"{kind}_prediction_rows"] / eligible if eligible else None
        metrics[f"{kind}_scoring_coverage"] = metrics[f"{kind}_scored_rows"] / eligible if eligible else None
    return metrics


def _aggregate(frame: pl.LazyFrame) -> dict:
    return _with_coverage(frame.select(_metrics()).collect(engine="streaming").to_dicts()[0])


def _group_metrics(frame: pl.LazyFrame, keys: list[str]) -> list[dict]:
    rows = frame.group_by(keys).agg(_metrics()).collect(engine="streaming").sort(keys).to_dicts()
    return [_with_coverage(row) for row in rows]


def _policy_metrics(frame: pl.LazyFrame) -> dict:
    """Each partition includes failures and missing metadata in explicit groups."""
    result = {key: _group_metrics(frame, [key, f"{key}_state"])
              for key in POLICY_METADATA_COLUMNS}
    result["profile_family"] = _group_metrics(
        frame, ["resolved_profile", "resolved_profile_state",
                "configured_curve", "configured_curve_state"])
    return result


def score(args: argparse.Namespace) -> dict:
    output = args.output.resolve()
    if output.exists():
        raise ValueError("score output already exists; use a new output path")
    _, receipt, joined = _load_evaluation(args.dataset, args.run_receipt, args.split, args.tick_size)
    metrics = _aggregate(joined)
    status = joined.group_by("input_status", "status").agg(pl.len().alias("rows"))
    result = {
        "schema_version": SCHEMA_VERSION, "split": args.split,
        "evaluator_sha256": ADAPTER_SHA256,
        "dataset_manifest_sha256": sha256(args.dataset / "manifest.json"),
        "run_receipt_sha256": sha256(args.run_receipt), "run": receipt,
        "tick_size": args.tick_size, "metrics": metrics,
        "status_counts": status.collect(engine="streaming").sort(["input_status", "status"]).to_dicts(),
        "by_dte_side": _group_metrics(joined, ["split", "dte_band", "is_call"]),
        "raw_strata": {key: _group_metrics(joined, [key]) for key in RAW_STRATA},
        "policy_metadata_states": POLICY_METADATA_STATES,
        "policy_strata": _policy_metrics(joined),
        "reference_nbbo_diagnostic": _group_metrics(joined, ["reference_location"]),
        "notes": ["srPrc/srVol are evaluation labels only",
                  "price errors are dollars per share; vol errors are absolute volatility basis points",
                  "spread-normalized errors divide by max(ask-bid,tick_size)",
                  "coverage denominators include every raw-input-eligible row, including solver failures",
                  "standard_symbol means no adjusted numeric suffix; it is not verified deliverable metadata",
                  "reference NBBO location is an evaluation diagnostic, never an admission rule",
                  "policy strata partition all selected rows, including explicit missing metadata groups",
                  "configured_curve describes selection; actual_curve may be absent after a fit failure",
                  "exact quantiles; scoring memory scales with selected cohort size"],
    }
    # Exclusive creation also closes the race between the initial existence
    # check and scoring. Neither aliases nor a concurrently created artifact
    # may be overwritten after it has served as provenance evidence.
    write_json(output, result, overwrite=False)
    print(json.dumps(metrics, allow_nan=False))
    return result


def _paired_metrics(kind: str) -> list[pl.Expr]:
    a_valid, b_valid = pl.col(f"{kind}_valid"), pl.col(f"{kind}_valid_b")
    common = a_valid & b_valid
    a = pl.when(common).then(pl.col(f"{kind}_error")).otherwise(None)
    b = pl.when(common).then(pl.col(f"{kind}_error_b")).otherwise(None)
    reduction = a.abs() - b.abs()
    prefix = "price" if kind == "price" else "vol_bp"
    eligible = pl.col("eligible").sum()
    a_predictions = pl.col(f"{kind}_prediction_valid").sum()
    b_predictions = pl.col(f"{kind}_prediction_valid_b").sum()
    return [common.sum().alias(f"{prefix}_intersection_rows"),
            a_predictions.alias(f"{prefix}_a_prediction_rows"),
            b_predictions.alias(f"{prefix}_b_prediction_rows"),
            pl.when(eligible > 0).then(a_predictions / eligible).otherwise(None)
              .alias(f"{prefix}_a_coverage"),
            pl.when(eligible > 0).then(b_predictions / eligible).otherwise(None)
              .alias(f"{prefix}_b_coverage"),
            (a_valid & ~b_valid).sum().alias(f"{prefix}_a_only_scored_rows"),
            (b_valid & ~a_valid).sum().alias(f"{prefix}_b_only_scored_rows"),
            *_error_metrics(a, f"{prefix}_a"), *_error_metrics(b, f"{prefix}_b"),
            reduction.mean().alias(f"{prefix}_paired_mean_abs_error_reduction"),
            reduction.median().alias(f"{prefix}_paired_median_abs_error_reduction"),
            (reduction > 0).mean().alias(f"{prefix}_fraction_improved"),
            (reduction < 0).mean().alias(f"{prefix}_fraction_worsened"),
            (reduction == 0).mean().alias(f"{prefix}_fraction_unchanged")]


def compare(args: argparse.Namespace) -> dict:
    """Compare exactly paired contracts; coverage remains visible for each arm."""
    output = args.output.resolve()
    if output.exists():
        raise ValueError("comparison output already exists; use a new output path")
    manifest, receipt_a, a = _load_evaluation(args.dataset, args.run_a, args.split, args.tick_size)
    _, receipt_b, b = _load_evaluation(args.dataset, args.run_b, args.split, args.tick_size)
    # Both arms were checked against this same manifest, including complete
    # row identities. Retain this explicit invariant in the comparison seam.
    if (receipt_a["quote_parquet_sha256"] != receipt_b["quote_parquet_sha256"]
            or receipt_a.get("quote_csv_sha256") != receipt_b.get("quote_csv_sha256")):
        raise ValueError("comparison arms have different quote input digests")
    b_columns = ["row_id", "price_valid", "vol_valid", "price_error", "vol_error",
                 "price_prediction_valid", "vol_prediction_valid",
                 *POLICY_METADATA_COLUMNS,
                 *[f"{name}_state" for name in POLICY_METADATA_COLUMNS]]
    paired = a.join(b.select(b_columns), on="row_id", how="left", validate="1:1", suffix="_b")
    paired = paired.rename({name: f"{name}_a" for key in POLICY_METADATA_COLUMNS
                            for name in (key, f"{key}_state")})
    pair_exprs = [pl.len().alias("rows"), pl.col("eligible").sum().alias("eligible_rows"),
                  *_paired_metrics("price"), *_paired_metrics("vol")]
    paired_metrics = paired.select(pair_exprs).collect(engine="streaming").to_dicts()[0]
    raw_strata = {key: paired.group_by(key).agg(pair_exprs).collect(engine="streaming")
                 .sort(key).to_dicts() for key in RAW_STRATA}
    policy_transitions = {}
    for key in POLICY_METADATA_COLUMNS:
        keys = [f"{key}_a", f"{key}_state_a", f"{key}_b", f"{key}_state_b"]
        policy_transitions[key] = (paired.group_by(keys).agg(pair_exprs)
                                   .collect(engine="streaming").sort(keys).to_dicts())
    result = {
        "schema_version": SCHEMA_VERSION, "split": args.split, "tick_size": args.tick_size,
        "evaluator_sha256": ADAPTER_SHA256,
        "dataset_manifest_sha256": sha256(args.dataset / "manifest.json"),
        "quote_parquet_sha256": manifest["files"]["quotes.parquet"]["sha256"],
        "arm_a": {"run_receipt_sha256": sha256(args.run_a), "run": receipt_a,
                  "metrics_own_population": _aggregate(a), "policy_strata": _policy_metrics(a)},
        "arm_b": {"run_receipt_sha256": sha256(args.run_b), "run": receipt_b,
                  "metrics_own_population": _aggregate(b), "policy_strata": _policy_metrics(b)},
        "paired": paired_metrics, "raw_strata_paired": raw_strata,
        "policy_metadata_states": POLICY_METADATA_STATES,
        "policy_transitions_paired": policy_transitions,
        "notes": ["paired metrics use the exact intersection of successful, finite, label-valid rows",
                  "price and volatility intersections are computed independently",
                  "positive paired error reduction means arm B has smaller absolute error than arm A",
                  "own-population error changes cannot establish improvement when coverage changes",
                  "policy transitions retain all selected rows; each group reports its own eligible denominator",
                  "metadata and its absence are artifact provenance, not inferred from current defaults",
                  "equal profile names need not imply equal parameters; consult each run's policy definition",
                  "standard_symbol is a raw suffix classification, not certified deliverable metadata",
                  "validation labels must not select hyperparameters"],
    }
    write_json(output, result, overwrite=False)
    print(json.dumps(paired_metrics, allow_nan=False))
    return result


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare", help="stream raw parquet into a physically label-isolated cohort")
    prep.add_argument("--source", required=True, type=Path)
    prep.add_argument("--output", required=True, type=Path)
    prep.add_argument("--symbols", default="", help="optional comma-separated raw underlier keys")
    prep.add_argument("--max-underliers", type=int, help="deterministic hash-order cap, independent of labels")
    prep.add_argument("--split", choices=["development", "validation", "all"], default="development")
    prep.add_argument("--split-seed", default="atx-vol-v1")
    prep.add_argument("--validation-fraction", type=float, default=0.2)
    prep.add_argument("--bucket-minutes", type=int, default=30)
    prep.add_argument("--buckets", default="", help="comma-separated New York HHMM buckets; empty means all")
    prep.add_argument("--batch-size", type=int, default=131072)
    prep.add_argument("--csv", action="store_true", help="also emit native runner input quotes.csv")
    prep.set_defaults(func=prepare)
    run = sub.add_parser("run", help="execute native fitter with input/binary/output hash provenance")
    run.add_argument("--dataset", required=True, type=Path)
    run.add_argument("--binary", required=True, type=Path)
    run.add_argument("--output", required=True, type=Path)
    run.add_argument("--model", choices=["quote-slice", "common-markov"], default="quote-slice")
    run.add_argument("--progress", type=Path,
                     help="fresh durable common-markov JSONL progress journal (no resume)")
    selection = run.add_mutually_exclusive_group()
    selection.add_argument("--curve", choices=["svi", "essvi", "convex-dense", "spline-vol",
                                              "linear-variance", "c8-event"],
                           help="fixed curve for controlled experiments")
    selection.add_argument("--policy", help="auto (default) or a symbol/profile JSON file")
    selection.add_argument("--common-policy", help="auto or a dedicated common-markov policy JSON file")
    run.add_argument("--dividends", choices=list(DIVIDEND_CONVENTIONS), default="funding-pv",
                     help="aggregate cash-PV approximation convention, or none for continuous yield")
    run.add_argument("extra", nargs=argparse.REMAINDER, help="native options after --")
    run.set_defaults(func=run_native)
    scoring = sub.add_parser("score", help="score predictions after verifying their cohort/run provenance")
    scoring.add_argument("--dataset", required=True, type=Path)
    scoring.add_argument("--run-receipt", required=True, type=Path)
    scoring.add_argument("--output", required=True, type=Path)
    scoring.add_argument("--split", choices=["development", "validation", "all"], default="development")
    scoring.add_argument("--tick-size", type=float, default=0.01)
    scoring.set_defaults(func=score)
    comparison = sub.add_parser("compare", help="compare two runs on exact price/vol intersections")
    comparison.add_argument("--dataset", required=True, type=Path)
    comparison.add_argument("--run-a", required=True, type=Path, help="baseline run receipt")
    comparison.add_argument("--run-b", required=True, type=Path, help="candidate run receipt")
    comparison.add_argument("--output", required=True, type=Path)
    comparison.add_argument("--split", choices=["development", "validation", "all"], default="development")
    comparison.add_argument("--tick-size", type=float, default=0.01)
    comparison.set_defaults(func=compare)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        args.func(args)
        return 0
    except (ValueError, OSError, RuntimeError) as exc:
        print(f"quote-benchmark: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
