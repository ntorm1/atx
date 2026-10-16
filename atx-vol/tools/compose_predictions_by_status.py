#!/usr/bin/env python3
"""Compose one prediction CSV from an ordered model list using only row status.

Each output row is copied from the FIRST input whose status column marks the row
as served (default status value ``ok``). When no input serves a row, the first
input's (refusal) row is copied unchanged. Selection reads only the status
column: it never opens labels, reference prices, errors or quotes, so the
composition is a deterministic routing rule, not a fitted ensemble.

Inputs must share the identical row_id sequence (same dataset row key and order)
and the same chain_id per row. Every input column survives: the output carries
the union of input columns in first-appearance order, with an empty value where
the selected source lacks a column, plus ``source_model`` (the selected input's
label) and ``source_index`` (its zero-based position). Field text is copied
verbatim (no float re-formatting).

A run receipt next to the output (``<output>.run.json``) records the tool hash,
every input path/SHA256/label, per-source selection counts and the output
SHA256. When every input has a quote_benchmark run receipt (``--receipts``),
their prediction hashes are verified and the shared quote-input digests are
copied, so ``quote_benchmark.py score``/``compare`` accept the composed receipt.
Mixed-model rows are not jointly arbitrage-checked; the receipt says so.

Outputs and receipts are created exclusively; nothing is ever overwritten.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import sys

import polars as pl

SCHEMA_VERSION = 1
TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
REQUIRED_COLUMNS = ("row_id", "chain_id", "status")
ADDED_COLUMNS = ("source_model", "source_index")
QUOTE_DIGESTS = ("quote_parquet_sha256", "quote_csv_sha256")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_text_csv(path: Path) -> pl.DataFrame:
    """Every column as text; empty fields become null and are written back empty."""
    frame = pl.read_csv(path, infer_schema_length=0)
    missing = [name for name in REQUIRED_COLUMNS if name not in frame.columns]
    if missing:
        raise ValueError(f"{path}: missing required columns {missing}")
    clash = [name for name in ADDED_COLUMNS if name in frame.columns]
    if clash:
        raise ValueError(f"{path}: already contains composition columns {clash}")
    if frame["row_id"].null_count() or frame["row_id"].n_unique() != frame.height:
        raise ValueError(f"{path}: row_id must be non-null and unique")
    return frame


def compose(frames: list[pl.DataFrame], labels: list[str], served_status: str) -> tuple[pl.DataFrame, dict]:
    first = frames[0]
    for index, frame in enumerate(frames[1:], start=1):
        if frame.height != first.height:
            raise ValueError(f"input {index} has {frame.height} rows; input 0 has {first.height}")
        if not frame["row_id"].equals(first["row_id"]):
            raise ValueError(f"input {index} row_id sequence differs from input 0")
        if not frame["chain_id"].equals(first["chain_id"], null_equal=True):
            raise ValueError(f"input {index} chain_id differs from input 0 on some row")
    columns: list[str] = []
    for frame in frames:
        columns.extend(name for name in frame.columns if name not in columns)
    served = [(frame["status"] == served_status).fill_null(False) for frame in frames]
    # First served source wins; rows nobody serves fall back to input 0.
    choice = pl.Series("source_index", [len(frames)] * first.height, dtype=pl.Int64)
    for index in reversed(range(len(frames))):
        choice = pl.select(pl.when(served[index]).then(pl.lit(index, dtype=pl.Int64))
                           .otherwise(choice)).to_series().alias("source_index")
    fallback = choice == len(frames)
    choice = pl.select(pl.when(fallback).then(pl.lit(0, dtype=pl.Int64)).otherwise(choice)).to_series()
    out = {}
    for name in columns:
        value = pl.Series(name, [None] * first.height, dtype=pl.String)
        for index, frame in enumerate(frames):
            if name in frame.columns:
                value = pl.select(pl.when(choice == index).then(frame[name]).otherwise(value)).to_series()
        out[name] = value.alias(name)
    out["source_model"] = pl.Series("source_model", labels, dtype=pl.String).gather(choice).alias("source_model")
    out["source_index"] = choice.cast(pl.String).alias("source_index")
    result = pl.DataFrame(out)
    selected = []
    for index, label in enumerate(labels):
        mask = (choice == index) & ~fallback
        selected.append({"index": index, "label": label,
                         "served_rows_in_input": int(served[index].sum()),
                         "rows_selected_as_served": int(mask.sum())})
    fallback_status = (first.filter(fallback).group_by("status").len()
                       .sort(["len", "status"], descending=[True, False]).to_dicts())
    counts = {"rows": first.height, "served_rows": int((~fallback).sum()),
              "fallback_rows_from_input_0": int(fallback.sum()),
              "per_source": selected,
              "fallback_status_counts": [{"status": r["status"], "rows": r["len"]} for r in fallback_status]}
    return result, counts


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inputs", nargs="+", required=True, type=Path, help="ordered prediction CSVs")
    ap.add_argument("--labels", nargs="+", required=True, help="source_model label per input")
    ap.add_argument("--receipts", nargs="+", type=Path,
                    help="optional quote_benchmark run receipt per input (verifies hashes, enables scoring)")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--served-status", default="ok", help="status value that marks a served row")
    return ap


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if len(args.inputs) < 2:
            raise ValueError("composition requires at least two inputs")
        if len(args.labels) != len(args.inputs) or len(set(args.labels)) != len(args.labels):
            raise ValueError("--labels must name each input once, uniquely")
        if args.receipts is not None and len(args.receipts) != len(args.inputs):
            raise ValueError("--receipts must list one run receipt per input")
        output = args.output.resolve()
        receipt_path = Path(str(output) + ".run.json")
        inputs = [path.resolve() for path in args.inputs]
        if output.exists() or receipt_path.exists() or output in inputs:
            raise ValueError("output/receipt already exists or aliases an input; use a new path")
        hashes = [sha256(path) for path in inputs]
        digests: dict[str, str | None] = {}
        receipts = []
        if args.receipts is not None:
            for path, source, digest in zip(args.receipts, inputs, hashes):
                receipt = json.loads(path.read_text(encoding="utf-8"))
                if Path(receipt["predictions"]).resolve() != source:
                    raise ValueError(f"{path}: receipt predictions path is not {source}")
                if receipt["prediction_sha256"] != digest:
                    raise ValueError(f"{path}: prediction hash disagrees with {source}")
                for key in QUOTE_DIGESTS:
                    if digests.setdefault(key, receipt.get(key)) != receipt.get(key):
                        raise ValueError(f"inputs disagree on {key}")
                receipts.append({"path": str(path.resolve()), "sha256": sha256(path)})
        frames = [read_text_csv(path) for path in inputs]
        if [sha256(path) for path in inputs] != hashes:
            raise ValueError("an input changed while it was being read")
        result, counts = compose(frames, args.labels, args.served_status)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as stream:
            result.write_csv(stream, null_value="")
            stream.flush()
            os.fsync(stream.fileno())
        receipt = {
            "schema_version": SCHEMA_VERSION, "kind": "status-routed-composition",
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "tool": str(Path(__file__).resolve()), "tool_sha256": TOOL_SHA256,
            "selection_rule": f"first input whose status == {args.served_status!r}; "
                              "otherwise input 0's row unchanged",
            "selection_inputs": "status column only; labels, references and errors are never read",
            "inputs": [{"index": i, "label": label, "predictions": str(path), "sha256": digest,
                        **({"run_receipt": receipts[i]} if receipts else {})}
                       for i, (label, path, digest) in enumerate(zip(args.labels, inputs, hashes))],
            "counts": counts, "columns": result.columns,
            "predictions": str(output), "prediction_sha256": sha256(output),
            **({key: value for key, value in digests.items()} if receipts else {}),
            "model": "status-routed-composition",
            "notes": ["rows from different models are not jointly arbitrage-checked",
                      "development composition only; no default promotion"],
        }
        with receipt_path.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(receipt, indent=2, sort_keys=True, allow_nan=False) + "\n")
        print(json.dumps({"predictions": str(output), "run_receipt": str(receipt_path), **counts}))
        return 0
    except (ValueError, OSError, KeyError) as exc:
        print(f"compose-predictions: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
