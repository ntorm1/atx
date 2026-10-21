#!/usr/bin/env python3
"""Compose one prediction CSV from an ordered model list using only row status.

Each output row is copied from the FIRST input whose status column marks the row
as served: its status is one of the ``--served-status`` values (default ``ok``
only; pass ``--served-status ok ok_price_only`` to also route price-only rows,
which carry a certified price and a blank vol). When no input serves a row, the
first input's (refusal) row is copied unchanged. Selection reads only the status
column: it never opens labels, reference prices, errors or quotes, so the
composition is a deterministic routing rule, not a fitted ensemble.

Opt-in row routing (``--restrict INDEX=ROWS.csv``, once per input at most): the
restricted input may serve only the row_ids listed in ROWS.csv (a CSV with a
``row_id`` column, built beforehand from raw input columns, never from labels).
Listed rows keep the ordinary first-served rule; any other row skips that input,
and its refusal fallback is the first input allowed on the row instead of input
0. Without ``--restrict`` the output is byte-identical to the unrouted rule.

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
Mixed-model rows are not jointly arbitrage-checked unless the cross-source
certificate below is switched on; the receipt says which.

Opt-in cross-source shape certificate (``--calendar-certificate off|demote|refuse``,
default ``off`` = the output above byte for byte; ``--dataset DIR`` names the
quote cohort whose ``quotes.parquet`` supplies the row geometry). Routing by
status mixes sources inside one chain and across the expiries of one underlier,
and every single-source policy that is clean on its own can still break the
served no-arbitrage conditions jointly (dev200 C5: 24 convex + 2,511 calendar
breaks, each pairing two sources). After the status routing, the served price
set — status ``ok`` or ``ok_price_only`` with a finite ``predicted_price``, the
scorer's definition — is tested with exactly the served-shape conditions of
``quote_benchmark.py`` ``_shape_breaks`` / the knot-ladder ``check_served.py``
(tolerance 1e-6): per chain and side calls non-increasing and puts
non-decreasing in strike, each interior price at most the chord of its
neighbours, and across the expiries of one (underlier, root, snapshot) at a
strike and side served by both, the earlier price at most the later one. A break
whose nodes all come from ONE source is that source's own responsibility and is
left alone (the receipt counts it as single-source). A break whose nodes come
from different sources acts on the node with the LARGEST source index (the
lowest-priority source): every served row of that expiry side (chain_id,
is_call) routed to that source is, under ``demote``, re-routed to the next
input in the ordered list that serves it (status in ``--served-status`` and
allowed on the row) — or, when no later input serves it or under ``refuse``,
refused: status ``calendar_arbitrage`` for a calendar break and
``arbitrage_failure`` for a monotone / convex break, ``predicted_price`` and
``predicted_vol`` blanked, ``source_model`` / ``source_index`` kept. Passes
repeat until no cross-source break remains: at most one demote pass per input,
then refuse passes; each pass removes one source from every offending expiry
side, so the loop is bounded by the number of inputs. The certificate reads only
status, ``predicted_price`` and the quote geometry columns (row_id, chain_id,
underlier, root, snapshot_ns, expiry_days, strike, is_call); never bid / ask,
never ``labels.parquet``. The receipt's ``calendar_certificate`` block records
the mode, pairs checked, breaks found by kind and source pair, rows demoted /
refused and the residual breaks of the written output.

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
CERTIFICATE_MODES = ("off", "demote", "refuse")
# Served-price set, tolerance and conditions exactly as quote_benchmark.py (PRICE_SERVED_STATUSES,
# SHAPE_TOLERANCE, _shape_breaks) and the knot-ladder check_served.py define them.
PRICE_SERVED = ("ok", "ok_price_only")
SHAPE_TOLERANCE = 1.0e-6
SHAPE_KINDS = ("monotone", "convex", "calendar")
KIND_PRECEDENCE = ("calendar", "convex", "monotone")  # the status an expiry side targeted by several kinds gets
REFUSAL_STATUS = {"calendar": "calendar_arbitrage", "convex": "arbitrage_failure",
                  "monotone": "arbitrage_failure"}
GEOMETRY_COLUMNS = ("row_id", "chain_id", "underlier", "root", "snapshot_ns", "expiry_days", "strike",
                    "is_call")
NODE_KEY = ["chain_id", "underlier", "root", "snapshot_ns", "expiry_days", "is_call", "strike"]
CALENDAR_KEY = ["underlier", "root", "snapshot_ns", "is_call", "strike"]
SERVED_DEFINITION = ("status ok or ok_price_only with a finite predicted_price "
                     "(quote_benchmark.py PRICE_SERVED_STATUSES)")
CERTIFICATE_RULE = ("served rows are tested for the monotone / convex / calendar conditions of "
                    "check_served.py (tolerance 1e-6); a break whose nodes come from different sources "
                    "acts on the node with the largest source_index: every served row of that expiry "
                    "side (chain_id, is_call) routed to that source is demoted to the next input that "
                    "serves it or refused (calendar_arbitrage for calendar, arbitrage_failure for "
                    "monotone / convex; price and vol blanked, source_model kept); single-source breaks "
                    "are left alone; passes repeat until no cross-source break remains")


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


def read_row_list(path: Path, row_ids: pl.Series) -> pl.Series:
    """Boolean mask over ``row_ids``: True where the row_id is listed in ``path``."""
    listed = pl.read_csv(path, infer_schema_length=0)
    if "row_id" not in listed.columns:
        raise ValueError(f"{path}: missing required column row_id")
    ids = listed["row_id"]
    if ids.null_count() or ids.n_unique() != listed.height:
        raise ValueError(f"{path}: row_id must be non-null and unique")
    unknown = ids.filter(~ids.is_in(row_ids.implode()))
    if unknown.len():
        raise ValueError(f"{path}: {unknown.len()} row_id values are not in the inputs")
    return row_ids.is_in(ids.implode()).alias("allowed")


def parse_restrictions(specs: list[str] | None, n_inputs: int) -> dict[int, Path]:
    result: dict[int, Path] = {}
    for spec in specs or []:
        index_text, sep, path_text = spec.partition("=")
        if not sep or not index_text.strip().isdigit() or not path_text:
            raise ValueError(f"--restrict expects INDEX=ROWS.csv, got {spec!r}")
        index = int(index_text)
        if index >= n_inputs:
            raise ValueError(f"--restrict index {index} is out of range for {n_inputs} inputs")
        if index in result:
            raise ValueError(f"--restrict names input {index} more than once")
        result[index] = Path(path_text)
    return result


def status_clause(served_status: list[str]) -> str:
    """Receipt wording for the served-status test; one value keeps the historical text."""
    if len(served_status) == 1:
        return f"status == {served_status[0]!r}"
    return f"status is one of {tuple(served_status)!r}"


def selection_rule(served_status: list[str], routing: bool = False) -> str:
    """Receipt text; a single value without routing keeps the historical wording byte for byte."""
    if not routing:
        return f"first input whose {status_clause(served_status)}; otherwise input 0's row unchanged"
    return (f"first input allowed on the row whose {status_clause(served_status)}; "
            "otherwise the first allowed input's row unchanged")


def pick(values: list[pl.Series | None], choice: pl.Series, name: str,
         dtype: pl.DataType = pl.String) -> pl.Series:
    """Per row, the value of the input ``choice`` selects (null where that input has none)."""
    out = pl.Series(name, [None] * choice.len(), dtype=dtype)
    for index, value in enumerate(values):
        if value is not None:
            out = pl.select(pl.when(choice == index).then(value).otherwise(out)).to_series()
    return out.alias(name)


def read_geometry(dataset: Path, first: pl.DataFrame) -> pl.DataFrame:
    """Quote geometry of every input row, in input order, from ``quotes.parquet`` key columns only.

    Opens nothing but ``quotes.parquet`` and reads only ``GEOMETRY_COLUMNS`` (never bid / ask,
    never ``labels.parquet``). Every input row must have a quote row with the same chain_id.
    """
    path = dataset / "quotes.parquet"
    geometry = pl.read_parquet(path, columns=list(GEOMETRY_COLUMNS))
    if geometry["row_id"].null_count() or geometry["row_id"].n_unique() != geometry.height:
        raise ValueError(f"{path}: row_id must be non-null and unique")
    try:
        rows = pl.DataFrame({"pos": pl.Series(range(first.height), dtype=pl.Int64),
                             "row_id": first["row_id"].cast(pl.UInt64, strict=True),
                             "input_chain_id": first["chain_id"].cast(pl.UInt64, strict=True)})
    except pl.exceptions.PolarsError as exc:
        raise ValueError(f"input row_id / chain_id are not integers: {exc}") from exc
    joined = rows.join(geometry, on="row_id", how="left").sort("pos")
    missing = int(joined["underlier"].null_count())
    if missing:
        raise ValueError(f"{path}: {missing} input rows have no quote row")
    if int((joined["chain_id"] != joined["input_chain_id"]).sum()):
        raise ValueError(f"{path}: chain_id disagrees with the inputs on some row")
    return joined.drop("input_chain_id")


def shape_breaks(nodes: pl.DataFrame) -> tuple[pl.DataFrame, dict]:
    """Served-shape breaks of a node set (one price per chain, side and strike), as the scorer finds them.

    One row per break: kind, the (chain_id, is_call) of the node tested, the source index of every
    node involved, and the expiry side the certificate acts on (the node with the largest source
    index). Also returns the number of pairs / triples tested and the cross-source calendar pairs.
    """
    side = ["chain_id", "is_call"]
    local = nodes.sort(side + ["strike"]).with_columns(
        pl.col("price").shift(1).over(side).alias("price_left"),
        pl.col("price").shift(-1).over(side).alias("price_right"),
        pl.col("strike").shift(1).over(side).alias("strike_left"),
        pl.col("strike").shift(-1).over(side).alias("strike_right"),
        pl.col("source").shift(1).over(side).alias("source_left"),
        pl.col("source").shift(-1).over(side).alias("source_right"))
    step = (pl.when(pl.col("is_call") == 1).then(pl.col("price") - pl.col("price_left"))
            .otherwise(pl.col("price_left") - pl.col("price")))
    weight = ((pl.col("strike") - pl.col("strike_left"))
              / (pl.col("strike_right") - pl.col("strike_left")))
    excess = pl.col("price") - (pl.col("price_left") * (1.0 - weight) + pl.col("price_right") * weight)
    monotone = local.filter(step > SHAPE_TOLERANCE).select(
        pl.lit("monotone").alias("kind"), "chain_id", "is_call",
        pl.concat_list(["source_left", "source"]).alias("sources"),
        pl.col("chain_id").alias("target_chain_id"),
        pl.max_horizontal("source_left", "source").alias("target_source"))
    convex = local.filter(excess > SHAPE_TOLERANCE).select(
        pl.lit("convex").alias("kind"), "chain_id", "is_call",
        pl.concat_list(["source_left", "source", "source_right"]).alias("sources"),
        pl.col("chain_id").alias("target_chain_id"),
        pl.max_horizontal("source_left", "source", "source_right").alias("target_source"))
    pairs = nodes.select(*CALENDAR_KEY, "chain_id", "expiry_days", "price", "source").join(
        nodes.select(*CALENDAR_KEY, pl.col("chain_id").alias("chain_id_late"),
                     pl.col("expiry_days").alias("expiry_days_late"),
                     pl.col("price").alias("price_late"), pl.col("source").alias("source_late")),
        on=CALENDAR_KEY).filter(pl.col("expiry_days") < pl.col("expiry_days_late"))
    calendar = pairs.filter(pl.col("price") - pl.col("price_late") > SHAPE_TOLERANCE).select(
        pl.lit("calendar").alias("kind"), "chain_id", "is_call",
        pl.concat_list(["source", "source_late"]).alias("sources"),
        pl.when(pl.col("source_late") > pl.col("source")).then(pl.col("chain_id_late"))
          .otherwise(pl.col("chain_id")).alias("target_chain_id"),
        pl.max_horizontal("source", "source_late").alias("target_source"))
    checked = {"monotone_pairs_checked": int(local["price_left"].is_not_null().sum()),
               "convex_triples_checked": int((local["price_left"].is_not_null()
                                              & local["price_right"].is_not_null()).sum()),
               "calendar_pairs_checked": pairs.height,
               "calendar_cross_source_pairs": int((pairs["source"] != pairs["source_late"]).sum())}
    breaks = pl.concat([monotone, convex, calendar]).with_columns(
        (pl.col("sources").list.min() != pl.col("sources").list.max()).alias("cross"))
    return breaks, checked


def summarise_breaks(breaks: pl.DataFrame, labels: list[str], checked: dict, duplicates: int,
                     served_rows: int) -> dict:
    """Receipt counts of one pass: breaks by kind, by cross / single source and by source pair."""
    by_kind = dict.fromkeys(SHAPE_KINDS, 0)
    cross_by_kind = dict.fromkeys(SHAPE_KINDS, 0)
    by_pair: dict[str, dict[str, int]] = {}
    for kind, cross, sources in zip(breaks["kind"].to_list(), breaks["cross"].to_list(),
                                    breaks["sources"].to_list()):
        by_kind[kind] += 1
        cross_by_kind[kind] += int(cross)
        pair = "+".join(sorted({labels[source] for source in sources}))
        by_pair.setdefault(pair, dict.fromkeys(SHAPE_KINDS, 0))[kind] += 1
    total, cross_total = sum(by_kind.values()), sum(cross_by_kind.values())
    return {"served_rows": served_rows, "breaks": total, "breaks_by_kind": by_kind,
            "cross_source_breaks": cross_total, "cross_source_breaks_by_kind": cross_by_kind,
            "single_source_breaks": total - cross_total,
            "breaks_by_source_pair": {pair: by_pair[pair] for pair in sorted(by_pair)},
            "duplicate_strike_disagreements": duplicates, **checked}


def certify(frames: list[pl.DataFrame], labels: list[str], served_status: list[str],
            permitted: list[pl.Series], choice: pl.Series, geometry: pl.DataFrame,
            mode: str) -> tuple[pl.Series, pl.Series, dict]:
    """Cross-source shape certificate over the status-routed choice (module docstring, "Opt-in
    cross-source shape certificate"). Returns the final choice, the refusal status per row (null =
    not refused) and the receipt block. Deterministic: reads status, predicted_price and geometry only.
    """
    n = len(frames)
    for index, frame in enumerate(frames):
        if "predicted_price" not in frame.columns:
            raise ValueError(f"input {index} lacks predicted_price; the calendar certificate needs it")
    prices = [frame["predicted_price"].cast(pl.Float64, strict=False) for frame in frames]
    statuses = [frame["status"] for frame in frames]
    serves = [statuses[i].is_in(served_status).fill_null(False) & permitted[i] for i in range(n)]
    height = choice.len()
    initial = choice.clone()
    refusal = pl.Series("refusal", [None] * height, dtype=pl.String)
    demoted_by_pair: dict[str, int] = {}
    refused_by_pair: dict[str, int] = {}
    passes: list[dict] = []
    first: dict | None = None
    final: dict | None = None
    for pass_index in range(2 * n + 1):
        price = pick(prices, choice, "price", pl.Float64)
        status = pick(statuses, choice, "status")
        served = (status.is_in(PRICE_SERVED).fill_null(False) & price.is_finite().fill_null(False)
                  & refusal.is_null())
        nodes = (geometry.with_columns(price=price, source=choice, served=served)
                 .filter(pl.col("served")).sort(["price", "source"])
                 .group_by(NODE_KEY).agg(pl.col("price").first(), pl.col("source").first(),
                                         pl.col("price").max().alias("price_high")))
        duplicates = int((nodes["price_high"] - nodes["price"] > SHAPE_TOLERANCE).sum())
        breaks, checked = shape_breaks(nodes.select(*NODE_KEY, "price", "source"))
        summary = summarise_breaks(breaks, labels, checked, duplicates, int(served.sum()))
        if first is None:
            first = summary
        cross = breaks.filter(pl.col("cross"))
        if cross.is_empty():
            final = summary
            break
        action = "demote" if mode == "demote" and pass_index < n else "refuse"
        targets = (cross.group_by(["target_chain_id", "is_call", "target_source"])
                   .agg(pl.col("kind").unique().alias("kinds"),
                        pl.col("sources").explode().unique().alias("all_sources"))
                   .sort(["target_chain_id", "is_call", "target_source"]))
        kinds = [next(k for k in KIND_PRECEDENCE if k in ks) for ks in targets["kinds"].to_list()]
        pairs = ["+".join(sorted(labels[s] for s in srcs)) for srcs in targets["all_sources"].to_list()]
        targets = targets.select(pl.col("target_chain_id").alias("chain_id"), "is_call",
                                 pl.col("target_source").alias("source")).with_columns(
            kind=pl.Series(kinds, dtype=pl.String), pair=pl.Series(pairs, dtype=pl.String))
        rows = (geometry.select("pos", "chain_id", "is_call").with_columns(source=choice, served=served)
                .join(targets, on=["chain_id", "is_call", "source"], how="left").sort("pos"))
        hit = rows["kind"].is_not_null() & rows["served"]
        if action == "demote":
            # Next input after the current one that serves the row (status + restriction); n = none.
            following = pl.Series("next", [n] * height, dtype=pl.Int64)
            for index in reversed(range(n)):
                following = pl.select(pl.when((choice < index) & serves[index])
                                      .then(pl.lit(index, dtype=pl.Int64)).otherwise(following)).to_series()
            moved = hit & (following < n)
            choice = pl.select(pl.when(moved).then(following).otherwise(choice)).to_series().alias("source_index")
            refused_now = hit & ~moved
        else:
            moved = pl.Series("moved", [False] * height, dtype=pl.Boolean)
            refused_now = hit
        status_now = rows["kind"].replace_strict(REFUSAL_STATUS, default=None, return_dtype=pl.String)
        refusal = pl.select(pl.when(refused_now).then(status_now).otherwise(refusal)).to_series().alias("refusal")
        for mask, tally in ((moved, demoted_by_pair), (refused_now, refused_by_pair)):
            for pair, count in rows.filter(mask).group_by("pair").len().iter_rows():
                tally[pair] = tally.get(pair, 0) + int(count)
        passes.append({"pass": pass_index + 1, "action": action, "breaks": summary["breaks"],
                       "cross_source_breaks": summary["cross_source_breaks"],
                       "expiry_sides_targeted": targets.height,
                       "rows_demoted": int(moved.sum()), "rows_refused": int(refused_now.sum())})
    if first is None or final is None:
        raise ValueError("calendar certificate did not converge within the pass bound")
    refused = refusal.is_not_null()
    by_status = {status: int(count) for status, count in
                 pl.DataFrame({"s": refusal}).filter(refused).group_by("s").len().sort("s").iter_rows()}
    block = {"mode": mode, "tolerance": SHAPE_TOLERANCE, "served_definition": SERVED_DEFINITION,
             "rule": CERTIFICATE_RULE,
             "pairs_checked": first["calendar_pairs_checked"],
             "cross_source_pairs": first["calendar_cross_source_pairs"],
             "monotone_pairs_checked": first["monotone_pairs_checked"],
             "convex_triples_checked": first["convex_triples_checked"],
             "breaks_found": first["breaks"], "breaks_found_by_kind": first["breaks_by_kind"],
             "cross_source_breaks_found": first["cross_source_breaks"],
             "cross_source_breaks_found_by_kind": first["cross_source_breaks_by_kind"],
             "single_source_breaks_found": first["single_source_breaks"],
             "breaks_found_by_source_pair": first["breaks_by_source_pair"],
             "duplicate_strike_disagreements": first["duplicate_strike_disagreements"],
             "passes": passes,
             "rows_demoted": int(((choice != initial) & ~refused).sum()),
             "rows_demoted_by_source_pair": dict(sorted(demoted_by_pair.items())),
             "rows_refused": int(refused.sum()),
             "rows_refused_by_source_pair": dict(sorted(refused_by_pair.items())),
             "rows_refused_by_status": by_status, "refusal_status_by_kind": REFUSAL_STATUS,
             "residual_breaks": final["breaks"], "residual_breaks_by_kind": final["breaks_by_kind"],
             "residual_cross_source_breaks": final["cross_source_breaks"],
             "residual_single_source_breaks": final["single_source_breaks"],
             "served_rows_before": first["served_rows"], "served_rows_after": final["served_rows"]}
    return choice, refusal, block


def compose(frames: list[pl.DataFrame], labels: list[str], served_status: list[str],
            allowed: dict[int, pl.Series] | None = None,
            certificate: dict | None = None) -> tuple[pl.DataFrame, dict, dict | None]:
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
    allowed = allowed or {}
    every = pl.Series("allowed", [True] * first.height, dtype=pl.Boolean)
    permitted = [allowed.get(index, every) for index in range(len(frames))]
    raw_served = [frame["status"].is_in(served_status).fill_null(False) for frame in frames]
    served = [raw_served[index] & permitted[index] for index in range(len(frames))]
    # First served source wins; rows nobody serves fall back to the first input
    # allowed on the row (input 0 whenever no restriction is given).
    choice = pl.Series("source_index", [len(frames)] * first.height, dtype=pl.Int64)
    for index in reversed(range(len(frames))):
        choice = pl.select(pl.when(served[index]).then(pl.lit(index, dtype=pl.Int64))
                           .otherwise(choice)).to_series().alias("source_index")
    fallback = choice == len(frames)
    refusal = pl.Series("source_index", [len(frames)] * first.height, dtype=pl.Int64)
    for index in reversed(range(len(frames))):
        refusal = pl.select(pl.when(permitted[index]).then(pl.lit(index, dtype=pl.Int64))
                            .otherwise(refusal)).to_series().alias("source_index")
    orphan = int((fallback & (refusal == len(frames))).sum())
    if orphan:
        raise ValueError(f"{orphan} rows are allowed by no input; leave one input unrestricted")
    choice = pl.select(pl.when(fallback).then(refusal).otherwise(choice)).to_series()
    certified = None
    refused_status = None
    if certificate is not None:
        choice, refused_status, certified = certify(frames, labels, served_status, permitted, choice,
                                                   certificate["geometry"], certificate["mode"])
        certified = {"quotes": certificate["quotes"], "quotes_sha256": certificate["quotes_sha256"],
                     **certified}
    out = {}
    for name in columns:
        out[name] = pick([frame[name] if name in frame.columns else None for frame in frames], choice, name)
    if refused_status is not None:
        refused = refused_status.is_not_null()
        out["status"] = pl.select(pl.when(refused).then(refused_status)
                                  .otherwise(out["status"])).to_series().alias("status")
        for name in ("predicted_price", "predicted_vol"):
            if name in out:
                out[name] = pl.select(pl.when(refused).then(pl.lit(None, dtype=pl.String))
                                      .otherwise(out[name])).to_series().alias(name)
    out["source_model"] = pl.Series("source_model", labels, dtype=pl.String).gather(choice).alias("source_model")
    out["source_index"] = choice.cast(pl.String).alias("source_index")
    result = pl.DataFrame(out)
    selected = []
    for index, label in enumerate(labels):
        mask = (choice == index) & ~fallback
        entry = {"index": index, "label": label,
                 "served_rows_in_input": int(raw_served[index].sum()),
                 "rows_selected_as_served": int(mask.sum())}
        if index in allowed:
            entry |= {"rows_allowed": int(permitted[index].sum()),
                      "served_rows_allowed": int(served[index].sum()),
                      "fallback_rows_from_input": int((fallback & (choice == index)).sum())}
        selected.append(entry)
    fallback_status = (result.filter(fallback).group_by("status").len()
                       .sort(["len", "status"], descending=[True, False]).to_dicts())
    counts = {"rows": first.height, "served_rows": int((~fallback).sum()),
              "fallback_rows_from_input_0": int((fallback & (choice == 0)).sum()),
              "per_source": selected,
              "fallback_status_counts": [{"status": r["status"], "rows": r["len"]} for r in fallback_status]}
    if allowed:
        counts["fallback_rows"] = int(fallback.sum())
    return result, counts, certified


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inputs", nargs="+", required=True, type=Path, help="ordered prediction CSVs")
    ap.add_argument("--labels", nargs="+", required=True, help="source_model label per input")
    ap.add_argument("--receipts", nargs="+", type=Path,
                    help="optional quote_benchmark run receipt per input (verifies hashes, enables scoring)")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--served-status", nargs="+", default=["ok"],
                    help="status value(s) that mark a served row (default: ok)")
    ap.add_argument("--restrict", action="append", metavar="INDEX=ROWS.csv",
                    help="input INDEX may serve only the row_ids listed in ROWS.csv (column row_id); "
                         "other rows skip it, refusals fall back to the first allowed input; "
                         "at most once per input, repeatable")
    ap.add_argument("--calendar-certificate", choices=CERTIFICATE_MODES, default="off",
                    help="cross-source served-shape certificate after the status routing: off (default; "
                         "output unchanged), demote (re-route the lower-priority source's expiry side "
                         "to the next input that serves it, else refuse) or refuse (drop it: status "
                         "calendar_arbitrage / arbitrage_failure, price and vol blanked); needs --dataset")
    ap.add_argument("--dataset", type=Path,
                    help="quote cohort directory whose quotes.parquet supplies the row geometry "
                         "(row_id, chain_id, underlier, root, snapshot_ns, expiry_days, strike, is_call) "
                         "for --calendar-certificate demote|refuse; labels.parquet is never opened")
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
        if len(set(args.served_status)) != len(args.served_status) or not all(args.served_status):
            raise ValueError("--served-status values must be non-empty and distinct")
        mode = args.calendar_certificate
        if mode != "off" and args.dataset is None:
            raise ValueError("--calendar-certificate demote|refuse needs --dataset <quote cohort dir>")
        if mode == "off" and args.dataset is not None:
            raise ValueError("--dataset is read only by --calendar-certificate demote|refuse")
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
        restrictions = parse_restrictions(args.restrict, len(inputs))
        frames = [read_text_csv(path) for path in inputs]
        if [sha256(path) for path in inputs] != hashes:
            raise ValueError("an input changed while it was being read")
        allowed: dict[int, pl.Series] = {}
        routing = []
        for index, path in sorted(restrictions.items()):
            digest = sha256(path)
            allowed[index] = read_row_list(path, frames[0]["row_id"])
            if sha256(path) != digest:
                raise ValueError(f"{path}: row list changed while it was being read")
            routing.append({"index": index, "label": args.labels[index], "rows": str(path.resolve()),
                            "sha256": digest, "rows_allowed": int(allowed[index].sum())})
        certificate = None
        if mode != "off":
            quotes = (args.dataset / "quotes.parquet").resolve()
            quotes_digest = sha256(quotes)
            if digests.get("quote_parquet_sha256") not in (None, quotes_digest):
                raise ValueError(f"{quotes}: not the quote cohort the input receipts name")
            geometry = read_geometry(args.dataset, frames[0])
            if sha256(quotes) != quotes_digest:
                raise ValueError(f"{quotes}: changed while it was being read")
            certificate = {"mode": mode, "geometry": geometry, "quotes": str(quotes),
                           "quotes_sha256": quotes_digest}
        result, counts, certified = compose(frames, args.labels, args.served_status, allowed, certificate)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as stream:
            result.write_csv(stream, null_value="")
            stream.flush()
            os.fsync(stream.fileno())
        receipt = {
            "schema_version": SCHEMA_VERSION, "kind": "status-routed-composition",
            "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "tool": str(Path(__file__).resolve()), "tool_sha256": TOOL_SHA256,
            "selection_rule": selection_rule(args.served_status, routing=bool(routing)),
            "selection_inputs": ("status column only; labels, references and errors are never read"
                                 if not routing else
                                 "status column plus the restriction row lists; labels, references "
                                 "and errors are never read")
                                + ("" if certified is None else
                                   "; the calendar certificate also reads predicted_price and the "
                                   "dataset's quote geometry (quotes.parquet key columns), never labels"),
            **({"restrictions": routing} if routing else {}),
            **({"calendar_certificate": certified} if certified is not None else {}),
            "inputs": [{"index": i, "label": label, "predictions": str(path), "sha256": digest,
                        **({"run_receipt": receipts[i]} if receipts else {})}
                       for i, (label, path, digest) in enumerate(zip(args.labels, inputs, hashes))],
            "counts": counts, "columns": result.columns,
            "predictions": str(output), "prediction_sha256": sha256(output),
            **({key: value for key, value in digests.items()} if receipts else {}),
            "model": "status-routed-composition",
            "notes": [("rows from different models are not jointly arbitrage-checked"
                       if certified is None else
                       f"served rows are jointly shape-checked across sources (calendar certificate "
                       f"{mode}); calendar_certificate.residual_breaks reports what remains"),
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
