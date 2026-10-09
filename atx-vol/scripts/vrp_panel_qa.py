#!/usr/bin/env python3
"""Panel QA report for vrp_panel_v1/v2 TSVs (bev_label_factory --vrp-panel).

Reads one or more panel TSVs (`# key=value` meta lines, then the frozen
tab-separated header + data rows) and writes one markdown QA report over the
union of all rows. The SCHEMA IS READ FROM EACH FILE (line 1,
`# schema=vrp_panel_v<N>`) and its column tuple enforced exactly: v1 is the
frozen 18-column contract, v2 appends `iv_atmf_21d`. Mixing v1 and v2 inputs
is allowed -- v1 rows are filled with a NaN `iv_atmf_21d` so every tier sees
one row shape -- and the report names which files carried which schema.

Report sections:
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
    session-adjacency assumption, unlike 4b);
(6) HARD realized-vol plausibility tier: any row with
    rv_fwd_21d > MAX_PLAUSIBLE_RV_FWD exits 1. This is the PERMANENT guard
    against the round-1..3 defect class -- an unadjusted corporate action
    read as a genuine return -- and it deliberately has no opt-out flag (see
    the constant for the threshold's justification);
(7) report-only implied-leg stability: the day-over-day |delta| distribution
    of iv_fair_21d and, on a v2 panel, of iv_atmf_21d beside it. The strip is
    a quadrature over a refit wing policy and jitters far more than the ATM
    read it stands in for; publishing both makes that gap measurable instead
    of anecdotal.
Pure stdlib -- see atx-vol/scripts/README.md for the tier note.

CLI:
    python vrp_panel_qa.py <panel.tsv>... --out-md report.md
        [--max-t21-violations N]

Exit codes: 0 clean (t+21 findings alone never fail the run unless
--max-t21-violations is given and exceeded), 1 duplicate keys,
label-identity violations, implausible realized vol, or more than
--max-t21-violations t+21 coverage violations (report still written), 2 bad
args / malformed input file.

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

# vrp_panel_v2 = v1's 18 columns in the frozen order, then the ATM-forward
# implied leg (the point a traded AtmForward straddle is struck and marked at,
# as opposed to iv_fair_21d's OTM-strip variance-swap fair strike).
COLUMNS_V2 = COLUMNS + ("iv_atmf_21d",)

# Schema comment line -> the exact column tuple that file must carry.
SCHEMA_COLUMNS = {
    "# schema=vrp_panel_v1": COLUMNS,
    "# schema=vrp_panel_v2": COLUMNS_V2,
}

# Every column that parses as a float (all but the two identity strings).
NUMERIC_COLUMNS = tuple(c for c in COLUMNS if c not in ("symbol", "date"))
NUMERIC_COLUMNS_V2 = tuple(c for c in COLUMNS_V2 if c not in ("symbol", "date"))

# The two implied legs whose day-over-day stability section 7 publishes.
IMPLIED_LEG_COLUMNS = ("iv_fair_21d", "iv_atmf_21d")

# Corpus-assembly identity key: one row per (symbol, session). Raw-string
# comparison (never float round-tripping), like bev_label_qa.py.
DUP_KEY_COLUMNS = ("symbol", "date")

HORIZON_SESSIONS = 21
HORIZON_YEARS = 21 / 252
LABEL_IDENTITY_TOL = 1e-12  # abs or rel -- allows FMA-contraction noise only
RV_RECOMPUTE_TOL = 1e-9     # report-only adjacency-assuming recompute

# PERMANENT data-integrity threshold. Must equal kVrpMaxPlausibleRvFwd in
# analytics/vrp_panel.hpp -- the C++ builder refuses to WRITE a v2 panel above
# it, and this tier refuses to PASS any panel above it, including the frozen v1
# artifacts the builder's gate cannot reach retroactively.
#
# Justified on the round-1 SP100 panel (19,042 labeled rows, 102 names,
# 2025-08..2026-07): the largest clean value is 1.222 (ORCL 2025-08-27) and the
# mildest corrupt one is 5.79 (NOW), so 3.0 sits 2.5x above the worst honest
# observation and 1.9x below the mildest corruption -- a factor-4.7 gap with
# nothing in between, so the threshold is fitted to neither edge. On economics,
# 300% annualized over a 21-bar (20-return) window needs sum(r^2) >= 0.714:
# one session of |r| >= 0.845 (a -57% day) or twenty consecutive +-19%
# sessions. The most extreme single-session large-cap collapse on record (AIG,
# 2008-09-16, -61%) lands near 3.35, so an event of that severity trips this
# tier and demands human confirmation rather than silent ingestion. That is the
# intended behaviour, which is why the tier has no opt-out flag.
MAX_PLAUSIBLE_RV_FWD = 3.0


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


def detect_schema(path: Path, meta_lines: list[str]) -> str:
    """The schema comment line the file declares. Exactly one recognized
    `# schema=` line must be present -- a panel that does not say what it is,
    or says something this checker does not know, is malformed input (exit 2),
    never something to guess at from the column count."""
    declared = [ln.strip() for ln in meta_lines if ln.startswith("# schema=")]
    if not declared:
        raise ValueError(f"{path}: no '# schema=vrp_panel_v<N>' comment line")
    if len(declared) > 1:
        raise ValueError(f"{path}: {len(declared)} '# schema=' lines; expected exactly one")
    if declared[0] not in SCHEMA_COLUMNS:
        raise ValueError(
            f"{path}: unknown schema '{declared[0]}'; "
            f"known: {', '.join(sorted(SCHEMA_COLUMNS))}"
        )
    return declared[0]


def parse_tsv_file(path: Path) -> tuple[list[str], list[dict[str, str]], str]:
    """One panel TSV -> (header columns, rows as raw string dicts, schema
    line). The schema is READ FROM THE FILE and its column tuple then enforced
    EXACTLY -- both contracts are frozen, so any drift is malformed input
    (ValueError -> exit 2), not something to tolerate.

    v1 rows are widened with a NaN `iv_atmf_21d` so callers mixing v1 and v2
    panels see one row shape; the missing leg then simply reads as absent
    coverage rather than as a parse special case."""
    text = path.read_text(encoding="utf-8")
    all_lines = text.splitlines()
    meta = [line for line in all_lines if line.startswith("#")]
    schema = detect_schema(path, meta)
    expected = SCHEMA_COLUMNS[schema]
    horizon_lines = [ln for ln in all_lines if ln.startswith("# horizon_days=")]
    if horizon_lines and horizon_lines[0] != f"# horizon_days={HORIZON_SESSIONS}":
        raise ValueError(f"{path}: unexpected horizon line '{horizon_lines[0]}'")
    lines = [line for line in all_lines if line and not line.startswith("#")]
    if not lines:
        return [], [], schema
    reader = csv.reader(lines, delimiter="\t")
    header = next(reader)
    if tuple(header) != expected:
        raise ValueError(f"{path}: header does not match {schema[9:]} columns: {header}")
    rows: list[dict[str, str]] = []
    for row_num, record in enumerate(reader, start=1):
        if len(record) != len(header):
            raise ValueError(
                f"{path}: row {row_num}: expected {len(header)} column(s), got {len(record)}"
            )
        row = dict(zip(header, record))
        for col in COLUMNS_V2:
            row.setdefault(col, "nan")
        rows.append(row)
    return header, rows, schema


def load_rows(paths: list[Path]) -> tuple[list[dict[str, str]], dict[str, int], dict[str, str]]:
    """Loads and concatenates every input file's rows (each tagged with its
    source file under "_file"), plus per-file row counts and per-file schema."""
    all_rows: list[dict[str, str]] = []
    per_file_counts: dict[str, int] = {}
    per_file_schema: dict[str, str] = {}
    for path in paths:
        _header, rows, schema = parse_tsv_file(path)
        per_file_counts[str(path)] = len(rows)
        per_file_schema[str(path)] = schema[len("# schema=") :]
        for row in rows:
            row["_file"] = str(path)
            all_rows.append(row)
    return all_rows, per_file_counts, per_file_schema


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
    for col in NUMERIC_COLUMNS_V2:
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


def check_implausible_rv(rows: list[dict[str, str]]) -> dict[str, Any]:
    """HARD tier: no forward realized vol may exceed MAX_PLAUSIBLE_RV_FWD.

    This is the permanent guard against the defect class that invalidated
    rounds 1-3: an unadjusted corporate action enters the spot mirror as a
    genuine multi-hundred-percent return, poisons every forward window that
    straddles it, flips the panel mean label's SIGN, and dominates a
    squared-error objective. It is silent by construction -- the rows parse,
    the label identity holds, no NaN appears -- so nothing else in this report
    would catch it. See MAX_PLAUSIBLE_RV_FWD for the threshold's justification;
    the remedy is to supply the missing factor to `--splits`, never to relax
    the tier, which is why it has no threshold flag."""
    violations: list[str] = []
    n_checked = 0
    worst = 0.0
    per_symbol: dict[str, int] = {}
    for row in rows:
        rv = _getf(row, "rv_fwd_21d")
        if math.isnan(rv):
            continue
        n_checked += 1
        if rv > MAX_PLAUSIBLE_RV_FWD:
            worst = max(worst, rv)
            per_symbol[row["symbol"]] = per_symbol.get(row["symbol"], 0) + 1
            violations.append(f"{row['symbol']}/{row['date']}: rv_fwd_21d={rv!r}")
    return {
        "n_checked": n_checked,
        "violations": violations,
        "worst": worst,
        "per_symbol": dict(sorted(per_symbol.items())),
    }


def _quantile(sorted_values: list[float], q: float) -> float:
    """Nearest-rank quantile on an already-sorted list (no interpolation, so
    the reported number is always an OBSERVED delta, not a synthetic one)."""
    if not sorted_values:
        return float("nan")
    idx = min(int(q * len(sorted_values)), len(sorted_values) - 1)
    return sorted_values[idx]


def compute_implied_leg_stability(rows: list[dict[str, str]]) -> dict[str, dict[str, float]]:
    """REPORT-ONLY: the day-over-day |delta| distribution of each implied leg.

    Within each symbol, ordered by entry_ts_ns, take |x[t] - x[t-1]| over
    CONSECUTIVE EMITTED rows for every pair where both values are finite, and
    report the distribution in VOL POINTS (x100). Consecutive emitted rows are
    not always consecutive sessions -- a dropped session widens one gap -- but
    both legs are measured over exactly the same row pairs, so the comparison
    between them is apples to apples even where the absolute level is not.

    The point of publishing both: iv_fair_21d is a quadrature over a refit wing
    policy at a synthetic tenor, so its jitter is largely fit noise rather than
    market; iv_atmf_21d reads one fitted point. A real 21-day implied moves
    ~0.5-1.5 vol points a day, so the gap between the two rows below is a
    direct measurement of how much of the label is strip noise."""
    by_symbol: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_symbol.setdefault(row["symbol"], []).append(row)
    out: dict[str, dict[str, float]] = {}
    for col in IMPLIED_LEG_COLUMNS:
        deltas: list[float] = []
        for sym_rows in by_symbol.values():
            ordered = sorted(sym_rows, key=lambda r: int(r["entry_ts_ns"]))
            prev = float("nan")
            for row in ordered:
                cur = _getf(row, col)
                if not math.isnan(prev) and not math.isnan(cur):
                    deltas.append(abs(cur - prev) * 100.0)
                prev = cur
        deltas.sort()
        n = len(deltas)
        out[col] = {
            "n": float(n),
            "mean": (sum(deltas) / n) if n else float("nan"),
            "median": _quantile(deltas, 0.5),
            "p90": _quantile(deltas, 0.90),
            "p99": _quantile(deltas, 0.99),
            "max": deltas[-1] if n else float("nan"),
        }
    return out


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
    implausible: dict[str, Any],
    stability: dict[str, dict[str, float]],
    per_file_schema: dict[str, str],
) -> str:
    schemas = sorted(set(per_file_schema.values()))
    lines: list[str] = [f"# vrp_panel QA report ({', '.join(schemas) if schemas else 'no input'})", ""]

    lines += ["## 1. Row accounting", "", "Rows per file:", ""]
    for file, count in accounting["rows_per_file"].items():
        lines.append(f"- {file}: {count} ({per_file_schema.get(file, 'unknown schema')})")
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
    for col in NUMERIC_COLUMNS_V2:
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

    lines += ["## 6. Realized-vol plausibility (hard tripwire)", ""]
    lines.append(
        f"Checked {implausible['n_checked']} finite rv_fwd_21d value(s) against the "
        f"{MAX_PLAUSIBLE_RV_FWD:g} annualized ceiling."
    )
    if implausible["violations"]:
        lines += [
            "",
            f"{len(implausible['violations'])} row(s) exceed it (worst "
            f"{implausible['worst']:.4f}). A value in this range is not a market event: "
            "it is an unadjusted corporate action entering the spot mirror as a genuine "
            "return. Supply the missing factor via the panel builder's --splits reference; "
            "do NOT relax this tier.",
            "",
            "Affected symbols:",
            "",
        ]
        for symbol, count in implausible["per_symbol"].items():
            lines.append(f"- {symbol}: {count} row(s)")
        lines += ["", "Rows:", ""]
        for v in implausible["violations"][:50]:
            lines.append(f"- {v}")
        if len(implausible["violations"]) > 50:
            lines.append(f"- ... and {len(implausible['violations']) - 50} more")
    else:
        lines.append("")
        lines.append("No violations.")
    lines.append("")

    lines += [
        "## 7. Implied-leg day-over-day stability (report-only)",
        "",
        "|delta| between consecutive emitted rows of the same symbol, in VOL POINTS.",
        "",
        "| leg | n | mean | median | p90 | p99 | max |",
        "|---|---|---|---|---|---|---|",
    ]
    for col in IMPLIED_LEG_COLUMNS:
        s = stability[col]
        lines.append(
            f"| {col} | {int(s['n'])} | {_fmt(s['mean'], 3)} | {_fmt(s['median'], 3)} | "
            f"{_fmt(s['p90'], 3)} | {_fmt(s['p99'], 3)} | {_fmt(s['max'], 3)} |"
        )
    lines += [
        "",
        "iv_fair_21d is an OTM-strip quadrature over a refit wing policy at a synthetic "
        "21/252 tenor; iv_atmf_21d reads one fitted point (the strike an AtmForward "
        "straddle actually trades). A real 21-day implied moves ~0.5-1.5 vol points a "
        "day, so the excess in the strip row is fit noise carried directly into the "
        "label. Report-only: the target choice is the trainer's decision, not this "
        "checker's.",
        "",
    ]

    return "\n".join(lines)


def build_report(paths: list[Path]) -> tuple[str, bool, int]:
    """Returns (markdown report, hard failure flag, t+21 violation count).
    hard_failure covers the unconditionally fatal tiers (duplicate keys, label
    identity, implausible realized vol); the t+21 count is report-only data the
    CLI gates on iff --max-t21-violations was given."""
    rows, per_file_counts, per_file_schema = load_rows(paths)
    accounting = compute_row_accounting(rows, per_file_counts)
    coverage = compute_nan_coverage(rows)
    duplicates = find_duplicates(rows)
    identity = check_label_identity(rows)
    rv_recompute = recompute_forward_rv(rows)
    correlations = compute_correlations(rows)
    t21 = check_t21_successors(rows)
    implausible = check_implausible_rv(rows)
    stability = compute_implied_leg_stability(rows)
    report_md = render_markdown(
        accounting,
        coverage,
        duplicates,
        identity,
        rv_recompute,
        correlations,
        t21,
        implausible,
        stability,
        per_file_schema,
    )
    hard_failure = (
        bool(duplicates) or bool(identity["violations"]) or bool(implausible["violations"])
    )
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
            f"vrp_panel_qa: duplicate keys, label-identity violations and/or implausible "
            f"realized vol found, see {args.out_md}",
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
