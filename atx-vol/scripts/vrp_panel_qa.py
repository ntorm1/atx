#!/usr/bin/env python3
"""Panel QA report for vrp_panel_v1 TSVs (bev_label_factory --vrp-panel).

Reads one or more vrp_panel_v1 TSVs (`# key=value` meta lines, then the
frozen 18-column tab-separated header + data rows) and writes one markdown
QA report over the union of all rows:
(1) row accounting by file/symbol, including predict-time (NaN-label) tail
    rows; (2) per-column NaN coverage; (3) cross-file duplicate
    (symbol, date) key check (nonzero exit -- a double-covered root/window,
    a real corpus-assembly bug); (4) lookahead tripwires:
      a. HARD per-row label identity label == (rv_fwd^2 - iv_fair_21d^2)
         * (21/252) within 1e-12 (abs or rel) -- a violation means the
         emitted label was not derived from the emitted columns (exit 1);
      b. report-only forward-RV recomputation from the panel's own spot
         column (assumes within-symbol session adjacency, which a dropped
         session legitimately breaks -- so this NEVER affects the exit
         code, mirroring bev_label_qa.py's report-only tripwire tier);
      c. report-only Pearson corr(label, f5/f6);
(5) F1 t+21 session-coverage check, REPORT-ONLY BY DEFAULT (the check the
    round-1 SP100 experiment demanded): within each symbol, ordered by
    entry_ts_ns, every finite-label row should have a same-symbol row
    HORIZON_SESSIONS positions later. The trainer's build_vrp_observations
    tolerates violating rows BY DESIGN (per-row rejection, counted, never
    fatal -- round 2), and real multi-name panels carry them wherever a
    surface history is thin, so by default this section only REPORTS the
    violation count and the affected labeled rows (exit code untouched --
    expected thin-history attrition must not mask the genuinely fatal
    tiers 3/4a). Passing --max-t21-violations N opts into a hard gate:
    exit 1 iff the violation count EXCEEDS N (pure row counting, no
    session-adjacency assumption, unlike 4b).
Pure stdlib -- see atx-vol/scripts/README.md for the tier note.

CLI:
    python vrp_panel_qa.py <panel.tsv>... --out-md report.md
        [--max-t21-violations N]

Exit codes: 0 clean (t+21 findings alone never fail the run unless
--max-t21-violations is given and exceeded), 1 duplicate keys,
label-identity violations, or more than --max-t21-violations t+21
coverage violations (report still written), 2 bad args / malformed
input file.

Run: python -m pytest atx-vol/scripts/vrp_panel_qa_test.py -q
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Any

# vrp_panel_v1 (analytics/vrp_panel.hpp kVrpPanelColumnsV1), frozen order.
COLUMNS = (
    "symbol",
    "date",
    "entry_ts_ns",
    "spot",
    "iv_fair_21d",
    "iv_fair_63d",
    "rv_fwd_21d",
    "label",
    "f0_log_rv1",
    "f1_log_rv5",
    "f2_log_rv21",
    "f3_iv_level",
    "f4_term_slope",
    "f5_hv_iv_gap",
    "f6_vrp_lag",
    "f7_ret_21d",
    "f8_jump_recent",
    "f9_vov_63d",
)

# Every column that parses as a float (all but the two identity strings).
NUMERIC_COLUMNS = tuple(c for c in COLUMNS if c not in ("symbol", "date"))

# Corpus-assembly identity key: one row per (symbol, session). Raw-string
# comparison (never float round-tripping), like bev_label_qa.py.
DUP_KEY_COLUMNS = ("symbol", "date")

SCHEMA_LINE = "# schema=vrp_panel_v1"
HORIZON_SESSIONS = 21
HORIZON_YEARS = 21 / 252
LABEL_IDENTITY_TOL = 1e-12  # abs or rel -- allows FMA-contraction noise only
RV_RECOMPUTE_TOL = 1e-9     # report-only adjacency-assuming recompute


# ── Numeric helpers (mirrors bev_label_qa.py) ─────────────────────────────


def pearson(xs: list[float], ys: list[float]) -> float | None:
    """Pearson correlation, pairwise-excluding NaNs; None when fewer than 2
    paired points survive or either series is constant."""
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
    """One panel TSV -> (header columns, rows as raw string dicts). Requires
    the frozen schema comment line and the EXACT frozen column header --
    vrp_panel_v1 is a frozen contract, so any drift is malformed input
    (ValueError -> exit 2), not something to tolerate."""
    text = path.read_text(encoding="utf-8")
    all_lines = text.splitlines()
    if SCHEMA_LINE not in (line.strip() for line in all_lines if line.startswith("#")):
        raise ValueError(f"{path}: missing frozen '{SCHEMA_LINE}' comment line")
    horizon_lines = [ln for ln in all_lines if ln.startswith("# horizon_days=")]
    if horizon_lines and horizon_lines[0] != f"# horizon_days={HORIZON_SESSIONS}":
        raise ValueError(f"{path}: unexpected horizon line '{horizon_lines[0]}'")
    lines = [line for line in all_lines if line and not line.startswith("#")]
    if not lines:
        return [], []
    reader = csv.reader(lines, delimiter="\t")
    header = next(reader)
    if tuple(header) != COLUMNS:
        raise ValueError(f"{path}: header does not match frozen vrp_panel_v1 columns: {header}")
    rows: list[dict[str, str]] = []
    for row_num, record in enumerate(reader, start=1):
        if len(record) != len(header):
            raise ValueError(
                f"{path}: row {row_num}: expected {len(header)} column(s), got {len(record)}"
            )
        rows.append(dict(zip(header, record)))
    return header, rows


def load_rows(paths: list[Path]) -> tuple[list[dict[str, str]], dict[str, int]]:
    """Loads and concatenates every input file's rows (each tagged with its
    source file under "_file"), plus per-file row counts."""
    all_rows: list[dict[str, str]] = []
    per_file_counts: dict[str, int] = {}
    for path in paths:
        _header, rows = parse_tsv_file(path)
        per_file_counts[str(path)] = len(rows)
        for row in rows:
            row["_file"] = str(path)
            all_rows.append(row)
    return all_rows, per_file_counts


def _getf(row: dict[str, str], col: str) -> float:
    return float(row[col])


# ── Report sections ───────────────────────────────────────────────────────


def compute_row_accounting(
    rows: list[dict[str, str]], per_file_counts: dict[str, int]
) -> dict[str, Any]:
    per_symbol: dict[str, int] = {}
    n_predict_time = 0  # NaN label = tail/predict-time rows (kept by contract)
    for row in rows:
        per_symbol[row["symbol"]] = per_symbol.get(row["symbol"], 0) + 1
        if math.isnan(_getf(row, "label")):
            n_predict_time += 1
    return {
        "rows_per_file": dict(per_file_counts),
        "total_rows": len(rows),
        "rows_per_symbol": dict(sorted(per_symbol.items())),
        "predict_time_rows": n_predict_time,
    }


def compute_nan_coverage(rows: list[dict[str, str]]) -> dict[str, dict[str, Any]]:
    total = len(rows)
    coverage: dict[str, dict[str, Any]] = {}
    for col in NUMERIC_COLUMNS:
        nan_count = sum(1 for row in rows if math.isnan(_getf(row, col)))
        fraction = (nan_count / total) if total > 0 else float("nan")
        coverage[col] = {"nan_count": nan_count, "total": total, "fraction": fraction}
    return coverage


def find_duplicates(rows: list[dict[str, str]]) -> list[tuple[tuple[str, ...], list[str]]]:
    """(symbol, date) keys seen more than once, with their source files."""
    seen: dict[tuple[str, ...], list[str]] = {}
    for row in rows:
        key = tuple(row[col] for col in DUP_KEY_COLUMNS)
        seen.setdefault(key, []).append(row.get("_file", ""))
    return sorted((key, files) for key, files in seen.items() if len(files) > 1)


def check_label_identity(rows: list[dict[str, str]]) -> dict[str, Any]:
    """HARD tripwire: within every row, label must equal
    (rv_fwd^2 - iv_fair_21d^2) * (21/252), and label/rv_fwd NaN-ness must
    agree (a NaN forward window is the ONLY sanctioned NaN-label reason)."""
    n_checked = 0
    violations: list[str] = []
    max_err = 0.0
    for row in rows:
        label = _getf(row, "label")
        rv = _getf(row, "rv_fwd_21d")
        iv = _getf(row, "iv_fair_21d")
        key = f"{row['symbol']}/{row['date']}"
        if math.isnan(rv) != math.isnan(label):
            violations.append(f"{key}: rv_fwd/label NaN-ness disagrees")
            continue
        if math.isnan(label):
            continue
        n_checked += 1
        expected = (rv * rv - iv * iv) * HORIZON_YEARS
        err = abs(label - expected)
        max_err = max(max_err, err)
        if err > LABEL_IDENTITY_TOL * max(1.0, abs(expected)):
            violations.append(f"{key}: label={label!r} expected={expected!r}")
    return {"n_checked": n_checked, "max_abs_err": max_err, "violations": violations}


def recompute_forward_rv(rows: list[dict[str, str]]) -> dict[str, Any]:
    """REPORT-ONLY lookahead tripwire: rebuild rv_fwd_21d at row t from the
    panel's own spot column over the next HORIZON rows of the same symbol
    (the exact C++ construction when within-symbol rows are consecutive
    sessions; a dropped session between rows breaks adjacency, which is why
    this never touches the exit code)."""
    by_symbol: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_symbol.setdefault(row["symbol"], []).append(row)
    n_checked = 0
    n_matched = 0
    max_abs_diff = 0.0
    for sym_rows in by_symbol.values():
        sym_rows.sort(key=lambda r: r["date"])
        closes = [_getf(r, "spot") for r in sym_rows]
        n = len(closes)
        for t, row in enumerate(sym_rows):
            rv = _getf(row, "rv_fwd_21d")
            if math.isnan(rv) or t + HORIZON_SESSIONS > n - 1:
                continue
            # bars t+1..t+21 -> the 20 c2c terms r_{t+2}..r_{t+21}.
            acc = 0.0
            for j in range(t + 2, t + HORIZON_SESSIONS + 1):
                r = math.log(closes[j] / closes[j - 1])
                acc += r * r
            rv_re = math.sqrt(acc / (HORIZON_SESSIONS - 1) * 252.0)
            n_checked += 1
            diff = abs(rv_re - rv)
            max_abs_diff = max(max_abs_diff, diff)
            if diff <= RV_RECOMPUTE_TOL:
                n_matched += 1
    return {"n_checked": n_checked, "n_matched": n_matched, "max_abs_diff": max_abs_diff}


def check_t21_successors(rows: list[dict[str, str]]) -> dict[str, Any]:
    """REPORT-ONLY tripwire by default (F1): within each symbol, ordered by
    entry_ts_ns, every finite-label row should have a same-symbol row
    HORIZON_SESSIONS positions later. The panel builder only labels a session
    whose forward window fits the bar axis, so a labeled row missing its t+21
    EMITTED successor means sessions inside the final horizon were dropped
    (surface holes) -- rows the trainer tolerates by rejecting one by one,
    counted, never fatally (build_vrp_observations, round 2). This check
    makes that attrition loud BEFORE a training run without failing panels
    that are merely thin; --max-t21-violations N opts into exit 1 when the
    count exceeds N. Pure row counting: no session-adjacency assumption, so
    a mid-panel dropped session alone never trips it."""
    by_symbol: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_symbol.setdefault(row["symbol"], []).append(row)
    n_checked = 0
    violations: list[str] = []
    for symbol in sorted(by_symbol):
        sym_rows = by_symbol[symbol]
        sym_rows.sort(key=lambda r: int(r["entry_ts_ns"]))
        n = len(sym_rows)
        for p, row in enumerate(sym_rows):
            if math.isnan(_getf(row, "label")):
                continue
            n_checked += 1
            if p + HORIZON_SESSIONS > n - 1:
                violations.append(
                    f"{symbol}/{row['date']}: only {n - 1 - p} later same-symbol row(s), "
                    f"need {HORIZON_SESSIONS}"
                )
    return {"n_checked": n_checked, "violations": violations}


def compute_correlations(rows: list[dict[str, str]]) -> dict[str, float | None]:
    label = [_getf(row, "label") for row in rows]
    f5 = [_getf(row, "f5_hv_iv_gap") for row in rows]
    f6 = [_getf(row, "f6_vrp_lag") for row in rows]
    return {
        "corr_label_f5_hv_iv_gap": pearson(label, f5),
        "corr_label_f6_vrp_lag": pearson(label, f6),
    }


# ── Rendering ─────────────────────────────────────────────────────────────


def _fmt(x: float | None, prec: int = 6) -> str:
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "n/a"
    return f"{x:.{prec}f}"


def render_markdown(
    accounting: dict[str, Any],
    coverage: dict[str, dict[str, Any]],
    duplicates: list[tuple[tuple[str, ...], list[str]]],
    identity: dict[str, Any],
    rv_recompute: dict[str, Any],
    correlations: dict[str, float | None],
    t21: dict[str, Any],
) -> str:
    lines: list[str] = ["# vrp_panel_v1 QA report", ""]

    lines += ["## 1. Row accounting", "", "Rows per file:", ""]
    for file, count in accounting["rows_per_file"].items():
        lines.append(f"- {file}: {count}")
    lines += ["", f"Total rows: {accounting['total_rows']}", "", "Rows per symbol:", ""]
    for symbol, count in accounting["rows_per_symbol"].items():
        lines.append(f"- {symbol}: {count}")
    lines += [
        "",
        f"Predict-time rows (NaN label, kept by contract): "
        f"{accounting['predict_time_rows']}",
        "",
    ]

    lines += [
        "## 2. NaN coverage",
        "",
        "| column | nan_count | total | nan_fraction |",
        "|---|---|---|---|",
    ]
    for col in NUMERIC_COLUMNS:
        c = coverage[col]
        lines.append(f"| {col} | {c['nan_count']} | {c['total']} | {_fmt(c['fraction'])} |")
    lines.append("")

    lines += ["## 3. Duplicate (symbol, date) check", ""]
    if not duplicates:
        lines.append("No duplicate (symbol, date) keys found.")
    else:
        lines.append(
            f"{len(duplicates)} duplicate key(s) found -- a root/window was double-covered "
            "(real corpus-assembly bug):"
        )
        lines.append("")
        for key, files in duplicates:
            symbol, date = key
            lines.append(
                f"- symbol={symbol}, date={date} "
                f"({len(files)} occurrences: {', '.join(files)})"
            )
    lines.append("")

    lines += ["## 4a. Label identity (hard tripwire)", ""]
    lines.append(
        f"Checked {identity['n_checked']} finite-label row(s); "
        f"max |label - (rv^2 - iv^2) * (21/252)| = {identity['max_abs_err']:.3e}."
    )
    if identity["violations"]:
        lines.append("")
        lines.append(f"{len(identity['violations'])} violation(s):")
        lines.append("")
        for v in identity["violations"][:50]:
            lines.append(f"- {v}")
        if len(identity["violations"]) > 50:
            lines.append(f"- ... and {len(identity['violations']) - 50} more")
    else:
        lines.append("No violations.")
    lines.append("")

    lines += ["## 4b. Forward-RV recompute (report-only lookahead tripwire)", ""]
    lines.append(
        f"Recomputed rv_fwd_21d from the panel's own spot column for "
        f"{rv_recompute['n_checked']} row(s) (within-symbol adjacency assumed): "
        f"{rv_recompute['n_matched']} matched within {RV_RECOMPUTE_TOL:g}; "
        f"max |diff| = {rv_recompute['max_abs_diff']:.3e}."
    )
    lines += [
        "",
        "Note: a dropped session (invalid surface / OutOfRange at 21d) between two "
        "emitted rows legitimately breaks the adjacency assumption, so mismatches "
        "here are diagnostic, never a failure (report-only, exit code untouched).",
        "",
    ]

    lines += ["## 4c. Correlations (report-only)", ""]
    lines.append(f"corr(label, f5_hv_iv_gap) = {_fmt(correlations['corr_label_f5_hv_iv_gap'])}")
    lines.append(f"corr(label, f6_vrp_lag) = {_fmt(correlations['corr_label_f6_vrp_lag'])}")
    lines += [
        "",
        "Note: |corr| near 1.0 suggests target leakage into features. Report-only; "
        "the trainer's own leakage audit owns that judgment.",
        "",
    ]

    lines += ["## 5. F1 t+21 session-coverage (report-only by default)", ""]
    lines.append(
        f"Checked {t21['n_checked']} finite-label row(s): each should have a "
        f"same-symbol row {HORIZON_SESSIONS} positions later (ordered by entry_ts_ns)."
    )
    if t21["violations"]:
        lines.append("")
        lines.append(
            f"{len(t21['violations'])} violation(s) -- the trainer rejects these rows "
            "one by one (per-row t+21 rejection, counted, never fatal; round 2). "
            "Report-only: the exit code is untouched unless --max-t21-violations "
            "is given and exceeded:"
        )
        lines.append("")
        for v in t21["violations"][:50]:
            lines.append(f"- {v}")
        if len(t21["violations"]) > 50:
            lines.append(f"- ... and {len(t21['violations']) - 50} more")
    else:
        lines.append("")
        lines.append("No violations.")
    lines.append("")

    return "\n".join(lines)


def build_report(paths: list[Path]) -> tuple[str, bool, int]:
    """Returns (markdown report, hard failure flag, t+21 violation count).
    hard_failure covers ONLY the unconditionally fatal tiers (duplicate keys,
    label identity); the t+21 count is report-only data the CLI gates on iff
    --max-t21-violations was given."""
    rows, per_file_counts = load_rows(paths)
    accounting = compute_row_accounting(rows, per_file_counts)
    coverage = compute_nan_coverage(rows)
    duplicates = find_duplicates(rows)
    identity = check_label_identity(rows)
    rv_recompute = recompute_forward_rv(rows)
    correlations = compute_correlations(rows)
    t21 = check_t21_successors(rows)
    report_md = render_markdown(
        accounting, coverage, duplicates, identity, rv_recompute, correlations, t21
    )
    hard_failure = bool(duplicates) or bool(identity["violations"])
    return report_md, hard_failure, len(t21["violations"])


# ── CLI ───────────────────────────────────────────────────────────────────


def _t21_threshold(text: str) -> int:
    """argparse type for --max-t21-violations: a non-negative int, rejected at
    PARSE time (fix-2 review minor: a sign typo like -1 previously turned a
    ZERO-violation panel into exit 1 with the absurd message "0 violation(s)
    exceed the -1 threshold" -- a usage error, exit 2, never a gate result)."""
    value = int(text)  # ValueError -> argparse usage error (exit 2)
    if value < 0:
        raise argparse.ArgumentTypeError(f"must be a non-negative count, got {value}")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a markdown QA report over one or more vrp_panel_v1 TSVs."
    )
    parser.add_argument("panels", nargs="+", type=Path, help="panel TSV file(s)")
    parser.add_argument("--out-md", required=True, type=Path)
    parser.add_argument(
        "--max-t21-violations",
        type=_t21_threshold,
        default=None,
        metavar="N",
        help="opt-in hard gate for section 5: exit 1 iff the t+21 coverage "
        "violation count EXCEEDS N (N >= 0; default: report-only, exit 0)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        report_md, hard_failure, n_t21 = build_report(args.panels)
    except (ValueError, OSError) as exc:
        print(f"vrp_panel_qa: {exc}", file=sys.stderr)
        return 2

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text(report_md, encoding="utf-8")

    if hard_failure:
        print(
            f"vrp_panel_qa: duplicate keys and/or label-identity violations found, "
            f"see {args.out_md}",
            file=sys.stderr,
        )
        return 1
    if args.max_t21_violations is not None and n_t21 > args.max_t21_violations:
        print(
            f"vrp_panel_qa: {n_t21} t+21 coverage violation(s) exceed the "
            f"--max-t21-violations {args.max_t21_violations} threshold, see {args.out_md}",
            file=sys.stderr,
        )
        return 1
    if n_t21 > 0:
        detail = (
            "report-only (no --max-t21-violations threshold)"
            if args.max_t21_violations is None
            else f"within the --max-t21-violations {args.max_t21_violations} threshold"
        )
        print(
            f"vrp_panel_qa: {n_t21} t+21 coverage violation(s), {detail}, "
            f"see {args.out_md}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
