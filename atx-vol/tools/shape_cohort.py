#!/usr/bin/env python3
"""Build the surface-shape x underlying classes of prepared quote cohorts from quote features only.

Plan docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md section 2 names nine classes. Each class
here is a list of (cohort, underlier, expiry) members written to ``<class>.csv``; seven are
symbol classes (every expiry of a member underlier) and two are expiry filters (short-dated,
long-dated) applied to every underlier. A member may belong to several classes; the manifest
records every pairwise overlap and the scorer (``quote_benchmark.py score --class-manifest``)
scores each class on all of its rows.

LABEL ISOLATION. Only the fixed FEATURE_COLUMNS of a cohort's ``quotes.parquet`` are read (the
file must match its cohort manifest and carry no label column); ``labels.parquet`` is never
opened, so srPrc/srVol cannot reach a predicate. The optional implied-forward report is a
quote-slice native report (``carry_resolutions``) computed from quotes; it must belong to the
cohort (every chain id in it is a chain of the cohort).

PREDICATES. Each class rule is ONE Python expression over the per-underlier (or per-expiry)
feature names below; the string in RULES is the code that runs (``eval`` with no builtins) and
the string the manifest records, so the published predicate cannot drift from the executed one.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys

import polars as pl
import pyarrow.parquet as pq


SCHEMA_VERSION = 1
TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
FEATURE_COLUMNS = ("chain_id", "snapshot_ns", "underlier", "root", "expiry_days", "strike",
                   "is_call", "spot", "bid", "ask", "years", "rate", "sdiv", "ddiv_pv",
                   "input_status")
LABEL_COLUMNS = ("srPrc", "srVol", "reference_price", "reference_vol")
CORE_ABS_LOG_MONEYNESS = 0.2      # the scorer's core moneyness band
STRADDLE_MIN_DTE, STRADDLE_MAX_DTE, STRADDLE_TARGET_DTE = 14, 60, 30
IMPLIED_MIN_CALENDAR_DAYS = 30.0  # shorter tenors turn bp forward noise into %/yr carry noise
FEATURE_DEFINITIONS = {
    "spot": "median spot (mid of the underlier bid/ask) over eligible rows",
    "rate": "median funding rate over eligible rows",
    "expiries": "distinct expiries with an eligible row",
    "expiries_within_7d": "distinct expiries with an eligible row and dte <= 7",
    "median_rows_per_expiry": "median over expiries of eligible rows per expiry",
    "core_rel_spread": (f"median (ask-bid)/mid over eligible rows with bid > 0 and "
                        f"|ln(K/S)| <= {CORE_ABS_LOG_MONEYNESS}; None when no such row"),
    "straddle_vol": (f"ATM straddle volatility proxy (C+P)*sqrt(2*pi)/2/(S*sqrt(years)) from quote "
                     f"mids, at the expiry with dte in [{STRADDLE_MIN_DTE}, {STRADDLE_MAX_DTE}] "
                     f"closest to {STRADDLE_TARGET_DTE} and the strike closest to spot with both "
                     "sides eligible and bid > 0; None when no such pair (never srVol, no IV solve)"),
    "has_cash_dividend": ("some row of the underlier (any input status) has finite ddiv_pv > 0 "
                          "(the composite-20261017 payer rule)"),
    "carry": ("rate - q, q = median implied_yield over the implied-forward report's applied "
              f"chains with calendar tenor >= {IMPLIED_MIN_CALENDAR_DAYS:g} days, else the median "
              "supplied sdiv over eligible rows (carry_source says which)"),
    "dte": "calendar days from the New York snapshot date to the expiry date",
}
# (name, plan label, plan representatives, rule). Symbol rules see the features above plus
# the earlier symbol classes' booleans; expiry rules see dte only.
SYMBOL_CLASSES = (
    ("index_etf_dense", "index / ETF, dense chains", ("SPY", "QQQ", "IWM"),
     "expiries_within_7d >= 4 and median_rows_per_expiry >= 200"),
    ("mega_cap_single", "mega-cap single name", ("AAPL", "MSFT", "NVDA", "AMZN"),
     "not index_etf_dense and spot >= 100 and expiries >= 12 and median_rows_per_expiry >= 100"
     " and core_rel_spread is not None and core_rel_spread <= 0.15"
     " and straddle_vol is not None and straddle_vol < 0.60"),
    ("high_vol", "high-vol / crypto-proxy", ("MSTR", "TSLA", "COIN"),
     "straddle_vol is not None and straddle_vol >= 0.60 and spot >= 20 and expiries >= 8"
     " and median_rows_per_expiry >= 40"),
    ("sparse", "sparse / new listing", ("CRWV",),
     "median_rows_per_expiry <= 24"),
    ("low_price_wide", "low-price, wide spread", ("RIVN", "SOFI", "F"),
     "spot < 20 and (core_rel_spread is None or core_rel_spread >= 0.05)"),
    ("dividend_payer", "discrete-dividend payers", ("ASML", "HSBC", "XOM"),
     "has_cash_dividend"),
    ("carry_abnormal", "negative / high carry, hard-to-borrow", ("GME", "AMC"),
     "carry is not None and (carry < -0.02 or carry > rate + 0.05)"),
)
EXPIRY_CLASSES = (
    ("short_dated", "short-dated (<= 3 DTE)", ("SPY", "QQQ"), "dte <= 3"),
    ("long_dated", "long-dated (> 365 DTE, LEAPS)", ("SPY", "AAPL"), "dte > 365"),
)
OVERLAP_RULE = ("classes may overlap: a member (underlier, expiry) belongs to every class whose "
                "rule it satisfies; expiry classes filter every underlier; manifest overlaps give "
                "shared rows per class pair and the scorer scores each class on all its rows")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def evaluate(rule: str, names: dict) -> bool:
    """Run one fixed in-module predicate; a non-bool result is a rule defect."""
    value = eval(rule, {"__builtins__": {}}, dict(names))  # noqa: S307 - constant module rules
    if not isinstance(value, bool):
        raise TypeError(f"rule did not return a bool: {rule}")
    return value


def load_quotes(dataset: Path) -> tuple[pl.DataFrame, dict]:
    """Feature columns of a verified cohort; refuse a label-bearing or mismatched quote file."""
    manifest_path = dataset / "manifest.json"
    quotes_path = dataset / "quotes.parquet"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    digest = sha256(quotes_path)
    if manifest.get("files", {}).get("quotes.parquet", {}).get("sha256") != digest:
        raise ValueError(f"{dataset}: quotes.parquet does not match its cohort manifest")
    names = set(pq.read_schema(quotes_path).names)
    if names & set(LABEL_COLUMNS):
        raise ValueError(f"{dataset}: quotes.parquet carries label columns {sorted(names & set(LABEL_COLUMNS))}")
    missing = set(FEATURE_COLUMNS) - names
    if missing:
        raise ValueError(f"{dataset}: quotes.parquet lacks feature columns {sorted(missing)}")
    quotes = pl.read_parquet(quotes_path, columns=list(FEATURE_COLUMNS))
    total = quotes.height
    quotes = quotes.filter(pl.col("underlier").is_not_null() & pl.col("expiry_days").is_not_null()
                           & pl.col("snapshot_ns").is_not_null())
    days = quotes.select(pl.col("snapshot_ns").cast(pl.Datetime("ns", "UTC"))
                         .dt.convert_time_zone("America/New_York").dt.date().unique())
    if days.height != 1:
        raise ValueError(f"{dataset}: one New York snapshot date per cohort is required")
    snapshot_day = days.item()
    quotes = quotes.with_columns((pl.col("expiry_days") - pl.lit(snapshot_day).cast(pl.Int32))
                                 .cast(pl.Int32).alias("dte"))
    info = {"dataset": str(dataset.resolve()), "quotes_parquet_sha256": digest,
            "dataset_manifest_sha256": sha256(manifest_path),
            "snapshot_date_new_york": snapshot_day.isoformat(),
            "rows_without_underlier_expiry_or_snapshot": total - quotes.height,
            "columns_read": list(FEATURE_COLUMNS), "labels_read": False}
    return quotes, info


def implied_carry(report_path: Path, quotes: pl.DataFrame) -> tuple[dict, dict]:
    """Median implied calendar yield per underlier from a quote-slice implied-forward report."""
    report = json.loads(report_path.read_text(encoding="utf-8"))
    resolutions = report.get("carry_resolutions") if isinstance(report, dict) else None
    if not isinstance(resolutions, list) or not resolutions:
        raise ValueError(f"{report_path}: not an implied-forward report (no carry_resolutions)")
    owner = dict(quotes.select("chain_id", "underlier").unique().iter_rows())
    yields: dict[str, list[float]] = {}
    applied = 0
    for record in resolutions:
        chain = int(record["chain_id"])
        if chain not in owner:
            raise ValueError(f"{report_path}: chain {chain} is not a chain of this cohort")
        q, years = record.get("implied_yield"), record.get("calendar_years")
        if record.get("applied") is not True:
            continue
        applied += 1
        if (isinstance(q, (int, float)) and math.isfinite(q) and isinstance(years, (int, float))
                and years * 365.25 >= IMPLIED_MIN_CALENDAR_DAYS):
            yields.setdefault(owner[chain], []).append(float(q))
    carry = {name: (statistics.median(values), len(values)) for name, values in yields.items()}
    info = {"path": str(report_path.resolve()), "sha256": sha256(report_path),
            "chains": len(resolutions), "applied_chains": applied,
            "chains_used": sum(len(v) for v in yields.values()), "underliers_used": len(carry),
            "min_calendar_days": IMPLIED_MIN_CALENDAR_DAYS}
    return carry, info


def straddle_vols(eligible: pl.DataFrame) -> dict:
    pairs = eligible.filter((pl.col("dte") >= STRADDLE_MIN_DTE) & (pl.col("dte") <= STRADDLE_MAX_DTE)
                            & (pl.col("bid") > 0))
    keys = ["underlier", "root", "expiry_days", "strike"]
    calls = pairs.filter(pl.col("is_call") == 1).group_by(keys).agg(
        pl.col("mid").min().alias("call"), pl.col("dte").first(), pl.col("spot").median(),
        pl.col("years").median())
    puts = pairs.filter(pl.col("is_call") == 0).group_by(keys).agg(pl.col("mid").min().alias("put"))
    both = calls.join(puts, on=keys).with_columns(
        (pl.col("dte") - STRADDLE_TARGET_DTE).abs().alias("dte_gap"),
        (pl.col("strike") / pl.col("spot")).log().abs().alias("abs_k"))
    pick = both.filter(pl.col("years") > 0).sort(
        ["underlier", "dte_gap", "dte", "abs_k", "strike", "root"]).group_by(
        "underlier", maintain_order=True).first()
    pick = pick.with_columns(((pl.col("call") + pl.col("put")) * math.sqrt(2 * math.pi) / 2
                              / (pl.col("spot") * pl.col("years").sqrt())).alias("straddle_vol"))
    return {row[0]: (row[1], row[2]) for row in
            pick.select("underlier", "straddle_vol", "dte").iter_rows()}


def classify(name: str, quotes: pl.DataFrame, carry_report: dict | None) -> tuple[pl.DataFrame, dict]:
    """Per-underlier features and class booleans, plus per-expiry member rows."""
    eligible = quotes.filter(pl.col("input_status") == "ok").with_columns(
        ((pl.col("bid") + pl.col("ask")) * 0.5).alias("mid"),
        (pl.col("strike") / pl.col("spot")).log().alias("log_k"))
    per_expiry = quotes.group_by("underlier", "expiry_days").agg(
        pl.len().alias("rows"), (pl.col("input_status") == "ok").sum().alias("eligible_rows"),
        pl.col("dte").first())
    elig_exp = per_expiry.filter(pl.col("eligible_rows") > 0).group_by("underlier").agg(
        pl.len().alias("expiries"), (pl.col("dte") <= 7).sum().alias("expiries_within_7d"),
        pl.col("eligible_rows").median().alias("median_rows_per_expiry"))
    core = eligible.filter((pl.col("bid") > 0) & (pl.col("log_k").abs() <= CORE_ABS_LOG_MONEYNESS)) \
        .group_by("underlier").agg(((pl.col("ask") - pl.col("bid")) / pl.col("mid")).median()
                                   .alias("core_rel_spread"))
    level = eligible.group_by("underlier").agg(
        pl.len().alias("eligible_rows"), pl.col("spot").median(), pl.col("rate").median(),
        pl.col("sdiv").median().alias("q_supplied"))
    cash = quotes.group_by("underlier").agg(
        pl.len().alias("rows"),
        (pl.col("ddiv_pv").is_not_null() & pl.col("ddiv_pv").is_finite() & (pl.col("ddiv_pv") > 0))
        .any().alias("has_cash_dividend"))
    straddle = straddle_vols(eligible)
    frame = cash.join(level, on="underlier", how="left").join(elig_exp, on="underlier", how="left") \
        .join(core, on="underlier", how="left").sort("underlier")
    records = []
    for row in frame.to_dicts():
        u = row["underlier"]
        sv = straddle.get(u, (None, None))
        q_implied, used = (carry_report or {}).get(u, (None, 0))
        q = q_implied if q_implied is not None else row["q_supplied"]
        rec = {"cohort": name, **row, "straddle_vol": sv[0], "straddle_dte": sv[1],
               "q_implied": q_implied, "implied_chains": used,
               "carry_source": ("implied_forward_report" if q_implied is not None
                                else "supplied_input" if row["q_supplied"] is not None else None),
               "carry": (row["rate"] - q) if row["rate"] is not None and q is not None else None,
               "carry_supplied": (row["rate"] - row["q_supplied"])
               if row["rate"] is not None and row["q_supplied"] is not None else None}
        classified = bool(row["eligible_rows"]) and row["spot"] is not None
        rec["classified"] = classified
        names = dict(rec)
        for cls, _label, _reps, rule in SYMBOL_CLASSES:
            names[cls] = rec[cls] = evaluate(rule, names) if classified else False
        records.append(rec)
    features = pl.DataFrame(records, infer_schema_length=None)
    members = per_expiry.with_columns(pl.lit(name).alias("cohort"))
    return features, {"per_expiry": members}


def class_members(features: pl.DataFrame, per_expiry: pl.DataFrame) -> dict[str, pl.DataFrame]:
    out = {}
    for cls, _label, _reps, _rule in SYMBOL_CLASSES:
        names = features.filter(pl.col(cls)).select("underlier")
        out[cls] = per_expiry.join(names, on="underlier", how="semi")
    for cls, _label, _reps, rule in EXPIRY_CLASSES:
        keep = [evaluate(rule, {"dte": d}) for d in per_expiry["dte"].to_list()]
        out[cls] = per_expiry.filter(pl.Series(keep, dtype=pl.Boolean))
    return out


def _iso(days: int) -> str:
    return (dt.date(1970, 1, 1) + dt.timedelta(days=int(days))).isoformat()


def build(args: argparse.Namespace) -> dict:
    cohorts = dict(_pair(spec, "--cohort") for spec in args.cohort)
    reports = dict(_pair(spec, "--implied-forward") for spec in args.implied_forward)
    if not cohorts:
        raise ValueError("at least one --cohort NAME=DATASET is required")
    if set(reports) - set(cohorts):
        raise ValueError("--implied-forward names a cohort that was not given")
    output = args.output
    output.mkdir(parents=True, exist_ok=False)
    all_features: list[pl.DataFrame] = []
    all_members: dict[str, list[pl.DataFrame]] = {}
    cohort_info: dict[str, dict] = {}
    carry_checks: dict[str, dict] = {}
    for name, dataset in cohorts.items():
        quotes, info = load_quotes(Path(dataset))
        carry, report_info = (implied_carry(Path(reports[name]), quotes) if name in reports
                              else ({}, None))
        features, parts = classify(name, quotes, carry if name in reports else None)
        info.update(rows=quotes.height, underliers=quotes["underlier"].n_unique(),
                    eligible_rows=int((quotes["input_status"] == "ok").sum()),
                    unclassified_underliers=features.filter(~pl.col("classified"))["underlier"].to_list(),
                    implied_forward_report=report_info)
        cohort_info[name] = info
        carry_rule = dict((c, r) for c, _l, _p, r in SYMBOL_CLASSES)["carry_abnormal"]
        if report_info is not None:
            flips = [row["underlier"] for row in features.filter(pl.col("classified")).to_dicts()
                     if row["carry_source"] == "implied_forward_report"
                     and evaluate(carry_rule, {**row, "carry": row["carry_supplied"]})
                     != row["carry_abnormal"]]
            carry_checks[name] = {
                "underliers_with_implied_carry": int((features["carry_source"]
                                                      == "implied_forward_report").sum()),
                "carry_abnormal_differs_if_supplied_carry_were_used": flips}
        all_features.append(features)
        for cls, frame in class_members(features, parts["per_expiry"]).items():
            all_members.setdefault(cls, []).append(frame)
    features = pl.concat(all_features, how="diagonal_relaxed")
    feature_path = output / "features.csv"
    features.write_csv(feature_path)
    classes, member_frames = [], {}
    for kind, table in (("symbol", SYMBOL_CLASSES), ("expiry", EXPIRY_CLASSES)):
        for cls, label, reps, rule in table:
            frame = pl.concat(all_members[cls]).with_columns(
                pl.col("expiry_days").map_elements(_iso, return_dtype=pl.String).alias("expiry"))
            frame = frame.select("cohort", "underlier", "expiry", "expiry_days", "dte", "rows",
                                 "eligible_rows").sort("cohort", "underlier", "expiry_days")
            member_frames[cls] = frame
            path = output / f"{cls}.csv"
            frame.write_csv(path)
            counts = {}
            for cohort in cohorts:
                sub = frame.filter(pl.col("cohort") == cohort)
                counts[cohort] = {"underliers": sub["underlier"].n_unique(), "expiries": sub.height,
                                  "rows": int(sub["rows"].sum()),
                                  "eligible_rows": int(sub["eligible_rows"].sum())}
            reps_status = {}
            for rep in reps:
                found = [c for c in cohorts
                         if rep in features.filter(pl.col("cohort") == c)["underlier"].to_list()]
                member = [c for c in cohorts
                          if rep in frame.filter(pl.col("cohort") == c)["underlier"].to_list()]
                reps_status[rep] = ("member: " + ",".join(member) if member
                                    else "present, not a member: " + ",".join(found) if found
                                    else "absent from every cohort")
            classes.append({"name": cls, "kind": kind, "plan_label": label, "rule": rule,
                            "file": path.name, "sha256": sha256(path), "counts": counts,
                            "plan_representatives": reps_status})
    overlaps = {}
    for cohort in cohorts:
        keyed = {c: f.filter(pl.col("cohort") == cohort) for c, f in member_frames.items()}
        overlaps[cohort] = {
            a: {b: int(fa.join(fb.select("underlier", "expiry_days"), on=["underlier", "expiry_days"],
                               how="semi")["rows"].sum()) for b, fb in keyed.items()}
            for a, fa in keyed.items()}
    manifest = {
        "schema_version": SCHEMA_VERSION, "tool": "atx-vol/tools/shape_cohort.py",
        "tool_sha256": TOOL_SHA256, "cohorts": cohort_info,
        "label_isolation": "quotes.parquet FEATURE_COLUMNS only; labels.parquet never opened; "
                           "srPrc/srVol never read",
        "feature_definitions": FEATURE_DEFINITIONS,
        "features": {"file": feature_path.name, "sha256": sha256(feature_path)},
        "classes": classes, "overlap_rule": OVERLAP_RULE, "overlap_rows": overlaps,
        "carry_cross_check": carry_checks,
    }
    payload = json.dumps(manifest, indent=2, sort_keys=True, allow_nan=False) + "\n"
    with (output / "manifest.json").open("x", encoding="utf-8") as stream:
        stream.write(payload)
    summary = {c["name"]: c["counts"] for c in classes}
    print(json.dumps(summary, sort_keys=True))
    return manifest


def _pair(spec: str, flag: str) -> tuple[str, str]:
    name, sep, value = spec.partition("=")
    if not sep or not name or not value:
        raise ValueError(f"{flag} expects NAME=PATH, got {spec!r}")
    return name, value


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="classify cohorts and write class files plus manifest.json")
    b.add_argument("--cohort", action="append", default=[], help="NAME=prepared cohort folder (repeatable)")
    b.add_argument("--implied-forward", action="append", default=[],
                   help="NAME=quote-slice implied-forward native report of that cohort (optional)")
    b.add_argument("--output", required=True, type=Path, help="new folder (refuses an existing one)")
    b.set_defaults(func=build)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        args.func(args)
        return 0
    except (ValueError, OSError, TypeError) as exc:
        print(f"shape-cohort: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
