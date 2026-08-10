#!/usr/bin/env python3
"""Label-corpus QA report for bev_label_factory TSVs (atx-vol Task 5).

Reads one or more 22-column label TSVs (bev_label_factory.cpp's
append_rows_tsv layout: `# key=value` meta header, then a tab-separated
header + data rows) and writes one markdown QA report over the union of all
rows: (1) row accounting by file/flag/snapped, (2) log_ratio distribution
overall and by tenor x delta bucket, (3) per-feature-column NaN coverage,
(4) cross-file duplicate-key check (nonzero exit on any duplicate -- a
manifest double-covered range, a real corpus-assembly bug), (5) a
report-only Pearson leakage tripwire (never affects exit code; the
trainer's own leakage audit owns that judgment, roadmap Sec.4 S3). Pure
stdlib -- see atx-vol/scripts/README.md for the tier note.

CLI:
    python bev_label_qa.py <labels.tsv>... --out-md report.md

Exit codes: 0 clean, 1 duplicate keys found (report still written), 2 bad
args / malformed input file.

Run: python -m pytest atx-vol/scripts/bev_label_qa_test.py -q
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Any

# kFairVolFeatureSchemaV1 (theo.hpp), schema order -- LabelRow's 8 trailing
# feature columns.
FEATURE_COLUMNS = (
    "log_moneyness",
    "tenor_years",
    "market_vol",
    "rv_21d",
    "rv_63d",
    "iv_minus_rv",
    "n_events_to_expiry",
    "delta_abs",
)

# Corpus-assembly identity key. Compared as raw TSV strings (not parsed
# floats) so duplicate detection never depends on float round-tripping.
DUP_KEY_COLUMNS = ("entry_ts_ns", "uid", "expiry_ns", "strike", "side")

_REQUIRED_COLUMNS = frozenset(DUP_KEY_COLUMNS) | frozenset(FEATURE_COLUMNS) | {
    "flag",
    "snapped",
    "log_ratio",
}

# BevFlag (breakeven.hpp): Ok=0, NoBracket=1, ExercisedEarly=2, MaxIter=3.
# Display names only -- an unknown future value falls back to "flag=N".
_FLAG_NAMES = {0: "Ok", 1: "NoBracket", 2: "ExercisedEarly", 3: "MaxIter"}

# Ordered, mutually-exclusive band thresholds (upper-bound cutoffs).
_TENOR_BANDS: tuple[tuple[str, float], ...] = (
    ("tenor<=0.12", 0.12),
    ("tenor<=0.30", 0.30),
    ("tenor<=0.60", 0.60),
)
_TENOR_OVERFLOW_LABEL = "tenor>0.60"

_DELTA_BANDS: tuple[tuple[str, float], ...] = (
    ("delta<0.25", 0.25),
    ("delta[0.25,0.5)", 0.5),
)
_DELTA_OVERFLOW_LABEL = "delta>=0.5"


def tenor_band(tenor_years: float) -> str | None:
    """tenor_years -> one of the 4 exclusive bands, or None if NaN."""
    if math.isnan(tenor_years):
        return None
    for label, hi in _TENOR_BANDS:
        if tenor_years <= hi:
            return label
    return _TENOR_OVERFLOW_LABEL


def delta_band(delta_abs: float) -> str | None:
    """delta_abs -> one of the 3 exclusive bands, or None if NaN."""
    if math.isnan(delta_abs):
        return None
    for label, hi in _DELTA_BANDS:
        if delta_abs < hi:
            return label
    return _DELTA_OVERFLOW_LABEL


_ALL_TENOR_LABELS = tuple(label for label, _ in _TENOR_BANDS) + (_TENOR_OVERFLOW_LABEL,)
_ALL_DELTA_LABELS = tuple(label for label, _ in _DELTA_BANDS) + (_DELTA_OVERFLOW_LABEL,)
_ALL_BUCKET_LABELS = tuple(
    f"{t} x {d}" for t in _ALL_TENOR_LABELS for d in _ALL_DELTA_LABELS
)


# ── Numeric helpers ──────────────────────────────────────────────────────

# Exact two-pass mean/stddev (not Welford): row counts per file/bucket are
# small enough that a second pass over an already-materialized list is free
# and side-steps reasoning about Welford's running-update error. Population
# moments (/n, not /(n-1)) -- a descriptive summary of the observed set,
# not an estimate of a larger population.
def mean_std(values: list[float]) -> tuple[float, float]:
    n = len(values)
    if n == 0:
        return float("nan"), float("nan")
    mean = sum(values) / n
    if n == 1:
        return mean, 0.0
    var = sum((v - mean) ** 2 for v in values) / n
    return mean, math.sqrt(var)


# Percentile via linear interpolation between the two closest ranks
# (numpy's default 'linear' method), not nearest-rank -- smoother across
# the P5/P50/P95 triple this report prints side by side.
def percentile(sorted_values: list[float], p: float) -> float:
    n = len(sorted_values)
    if n == 0:
        return float("nan")
    if n == 1:
        return sorted_values[0]
    idx = (p / 100.0) * (n - 1)
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return sorted_values[int(idx)]
    frac = idx - lo
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * frac


def pearson(xs: list[float], ys: list[float]) -> float | None:
    """Pearson correlation, pairwise-excluding any (x, y) where either side
    is NaN. Returns None (rendered "n/a") when fewer than 2 paired points
    survive, or when either series has zero variance -- the denominator
    would be zero (a constant column has an undefined correlation, not a
    divide-by-zero bug)."""
    pairs = [(x, y) for x, y in zip(xs, ys) if not math.isnan(x) and not math.isnan(y)]
    n = len(pairs)
    if n < 2:
        return None
    mean_x = sum(p[0] for p in pairs) / n
    mean_y = sum(p[1] for p in pairs) / n
    cov = sum((x - mean_x) * (y - mean_y) for x, y in pairs)
    var_x = sum((x - mean_x) ** 2 for x, y in pairs)
    var_y = sum((y - mean_y) ** 2 for x, y in pairs)
    if var_x == 0.0 or var_y == 0.0:
        return None
    return cov / math.sqrt(var_x * var_y)


# ── TSV loading ───────────────────────────────────────────────────────────


def parse_tsv_file(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    """One label TSV -> (header columns, rows as raw string dicts). `#`
    meta-header lines are skipped wherever they appear (the driver always
    puts them first, but nothing here depends on that). A header-only file
    (or a file with no non-'#' lines at all) yields ([], []) or (header, [])
    -- an empty/near-empty input is valid, not an error."""
    text = path.read_text(encoding="utf-8")
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    if not lines:
        return [], []
    reader = csv.reader(lines, delimiter="\t")
    header = next(reader)
    rows = [dict(zip(header, record)) for record in reader]
    return header, rows


def load_rows(paths: list[Path]) -> tuple[list[dict[str, str]], dict[str, int]]:
    """Loads and concatenates every input file's rows (each tagged with its
    source file under "_file"), plus a path-order-preserving per-file row
    count for the row-accounting section."""
    all_rows: list[dict[str, str]] = []
    per_file_counts: dict[str, int] = {}
    for path in paths:
        header, rows = parse_tsv_file(path)
        if rows:
            missing = sorted(_REQUIRED_COLUMNS - set(header))
            if missing:
                raise ValueError(f"{path}: missing required column(s): {missing}")
        per_file_counts[str(path)] = len(rows)
        for row in rows:
            row["_file"] = str(path)
            all_rows.append(row)
    return all_rows, per_file_counts


def _getf(row: dict[str, str], col: str) -> float:
    return float(row[col])


# ── Report sections ──────────────────────────────────────────────────────


def compute_row_accounting(rows: list[dict[str, str]], per_file_counts: dict[str, int]) -> dict[str, Any]:
    rows_by_flag: dict[int, int] = {}
    rows_by_snapped: dict[int, int] = {}
    for row in rows:
        flag = int(row["flag"])
        rows_by_flag[flag] = rows_by_flag.get(flag, 0) + 1
        snapped = int(row["snapped"])
        rows_by_snapped[snapped] = rows_by_snapped.get(snapped, 0) + 1
    return {
        "rows_per_file": dict(per_file_counts),
        "total_rows": len(rows),
        "rows_by_flag": dict(sorted(rows_by_flag.items())),
        "rows_by_snapped": dict(sorted(rows_by_snapped.items())),
    }


def _bucket_stats(values: list[float]) -> dict[str, float]:
    mean, std = mean_std(values)
    ordered = sorted(values)
    return {
        "n": len(values),
        "mean": mean,
        "stddev": std,
        "p5": percentile(ordered, 5),
        "p50": percentile(ordered, 50),
        "p95": percentile(ordered, 95),
    }


def compute_target_distribution(rows: list[dict[str, str]]) -> dict[str, Any]:
    overall_values: list[float] = []
    bucketed: dict[str, list[float]] = {label: [] for label in _ALL_BUCKET_LABELS}
    for row in rows:
        log_ratio = _getf(row, "log_ratio")
        if math.isnan(log_ratio):
            continue
        overall_values.append(log_ratio)
        tband = tenor_band(_getf(row, "tenor_years"))
        dband = delta_band(_getf(row, "delta_abs"))
        if tband is None or dband is None:
            continue  # cannot place an undefined tenor/delta into a bucket
        bucketed[f"{tband} x {dband}"].append(log_ratio)
    return {
        "overall": _bucket_stats(overall_values),
        "buckets": {label: _bucket_stats(values) for label, values in bucketed.items()},
    }


def compute_feature_coverage(rows: list[dict[str, str]]) -> dict[str, dict[str, Any]]:
    total = len(rows)
    coverage: dict[str, dict[str, Any]] = {}
    for col in FEATURE_COLUMNS:
        nan_count = sum(1 for row in rows if math.isnan(_getf(row, col)))
        fraction = (nan_count / total) if total > 0 else float("nan")
        coverage[col] = {"nan_count": nan_count, "total": total, "fraction": fraction}
    return coverage


def find_duplicates(rows: list[dict[str, str]]) -> list[tuple[tuple[str, ...], list[str]]]:
    """Groups rows by the raw-string DUP_KEY_COLUMNS tuple; returns
    (key, source_files) for every key seen more than once, sorted by key for
    deterministic report output."""
    seen: dict[tuple[str, ...], list[str]] = {}
    for row in rows:
        key = tuple(row[col] for col in DUP_KEY_COLUMNS)
        seen.setdefault(key, []).append(row.get("_file", ""))
    return sorted((key, files) for key, files in seen.items() if len(files) > 1)


def compute_leakage(rows: list[dict[str, str]]) -> dict[str, float | None]:
    log_ratio = [_getf(row, "log_ratio") for row in rows]
    iv_minus_rv = [_getf(row, "iv_minus_rv") for row in rows]
    market_vol = [_getf(row, "market_vol") for row in rows]
    return {
        "corr_log_ratio_iv_minus_rv": pearson(log_ratio, iv_minus_rv),
        "corr_log_ratio_market_vol": pearson(log_ratio, market_vol),
    }


# ── Rendering ─────────────────────────────────────────────────────────────


def _fmt(x: float | None, prec: int = 6) -> str:
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "n/a"
    return f"{x:.{prec}f}"


def _fmt_stats_row(label: str, stats: dict[str, float]) -> str:
    return (
        f"| {label} | {stats['n']} | {_fmt(stats['mean'])} | {_fmt(stats['stddev'])} | "
        f"{_fmt(stats['p5'])} | {_fmt(stats['p50'])} | {_fmt(stats['p95'])} |"
    )


def render_markdown(
    accounting: dict[str, Any],
    distribution: dict[str, Any],
    coverage: dict[str, dict[str, Any]],
    duplicates: list[tuple[tuple[str, ...], list[str]]],
    leakage: dict[str, float | None],
) -> str:
    lines: list[str] = ["# Label-corpus QA report", ""]

    lines += ["## 1. Row accounting", "", "Rows per file:", ""]
    for file, count in accounting["rows_per_file"].items():
        lines.append(f"- {file}: {count}")
    lines += ["", f"Total rows: {accounting['total_rows']}", "", "Rows by flag:", ""]
    for flag, count in accounting["rows_by_flag"].items():
        name = _FLAG_NAMES.get(flag, f"flag={flag}")
        lines.append(f"- {flag} ({name}): {count}")
    lines += ["", "Rows by snapped:", ""]
    for snapped, count in accounting["rows_by_snapped"].items():
        lines.append(f"- {snapped}: {count}")
    lines.append("")

    lines += ["## 2. Target distribution (log_ratio)", ""]
    header = "| bucket | n | mean | stddev | p5 | p50 | p95 |"
    sep = "|---|---|---|---|---|---|---|"
    lines += [header, sep, _fmt_stats_row("overall", distribution["overall"])]
    for label in _ALL_BUCKET_LABELS:
        lines.append(_fmt_stats_row(label, distribution["buckets"][label]))
    lines.append("")

    lines += ["## 3. Feature coverage", "", "| column | nan_count | total | nan_fraction |", "|---|---|---|---|"]
    for col in FEATURE_COLUMNS:
        c = coverage[col]
        lines.append(f"| {col} | {c['nan_count']} | {c['total']} | {_fmt(c['fraction'])} |")
    lines.append("")

    lines += ["## 4. Duplicate check", ""]
    if not duplicates:
        lines.append("No duplicate (entry_ts_ns, uid, expiry_ns, strike, side) keys found.")
    else:
        lines.append(
            f"{len(duplicates)} duplicate key(s) found -- a manifest double-covered a range "
            "(real corpus-assembly bug):"
        )
        lines.append("")
        for key, files in duplicates:
            entry_ts_ns, uid, expiry_ns, strike, side = key
            lines.append(
                f"- entry_ts_ns={entry_ts_ns}, uid={uid}, expiry_ns={expiry_ns}, "
                f"strike={strike}, side={side} ({len(files)} occurrences: {', '.join(files)})"
            )
    lines.append("")

    lines += ["## 5. Leakage tripwire (report-only)", ""]
    lines.append(f"corr(log_ratio, iv_minus_rv) = {_fmt(leakage['corr_log_ratio_iv_minus_rv'])}")
    lines.append(f"corr(log_ratio, market_vol) = {_fmt(leakage['corr_log_ratio_market_vol'])}")
    lines += [
        "",
        "Note: |corr| near 1.0 suggests target leakage into features. This is "
        "report-only (no assertion, never affects exit code) -- the trainer's "
        "own leakage audit owns that judgment (roadmap Sec.4 S3).",
        "",
    ]

    return "\n".join(lines)


def build_report(paths: list[Path]) -> tuple[str, list[tuple[tuple[str, ...], list[str]]]]:
    rows, per_file_counts = load_rows(paths)
    accounting = compute_row_accounting(rows, per_file_counts)
    distribution = compute_target_distribution(rows)
    coverage = compute_feature_coverage(rows)
    duplicates = find_duplicates(rows)
    leakage = compute_leakage(rows)
    report_md = render_markdown(accounting, distribution, coverage, duplicates, leakage)
    return report_md, duplicates


# ── CLI ───────────────────────────────────────────────────────────────────


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a markdown QA report over one or more bev_label_factory label TSVs."
    )
    parser.add_argument("labels", nargs="+", type=Path, help="label TSV file(s)")
    parser.add_argument("--out-md", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        report_md, duplicates = build_report(args.labels)
    except ValueError as exc:
        print(f"bev_label_qa: {exc}", file=sys.stderr)
        return 2

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text(report_md, encoding="utf-8")

    if duplicates:
        print(
            f"bev_label_qa: {len(duplicates)} duplicate key(s) found, see {args.out_md}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
