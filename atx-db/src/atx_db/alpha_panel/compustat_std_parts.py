"""Stage ``compustat_std_parts`` (P9 AL-data request B3): the pinned ``compustat_std`` funda, one part per UTC year of
``available_at``, under a C5 (engine ``PinnedStage``) manifest; plus the B3 acceptance printers.

Rows (rule ``compustat-std-funda-v1``). Every row of the published ``compustat_std/funda.parquet`` (filing and
restatement vintages, leavers included; nothing filtered, nothing overwritten), projected to the 20 requested columns
(``COLUMNS``), values unchanged. A row with NULL ``available_at`` is never usable (request rule C1): it is excluded
and counted (``counts.excluded_null_available_at``, ``excluded``). The upstream stage is opened only when its
``manifest.json`` SHA-256 equals the pin (``UPSTREAM_MANIFEST_SHA256``) and ``funda.parquet`` matches the bytes and
SHA-256 that manifest lists; the source (size, mtime) must not move while it is read.

Parts (request rule C3). ``parts/year=YYYY/funda.parquet`` holds exactly the rows with ``available_at`` (naive
TIMESTAMP, UTC) in [YYYY-01-01, YYYY+1-01-01). A TRAIN build that never opens a part named 2024 or later never sees
a row stamped 2024-01-01 or later.

Manifest (request rule C5, ``survey-b.md`` section 0). ``schema``, ``status: "complete"``, ``rule`` (one string),
``clock_rule`` (string), ``staleness_days`` (JSON integer), ``files`` = ``{rel: {bytes, sha256}}`` for exactly the
parts this build wrote (never a directory glob), the upstream manifest SHA-256, code identity and per-part counts,
written last and atomically. A stage directory that already holds anything is never replaced (published outputs are
immutable): a rebuild takes a new ``--stage`` name.

Clock (``cstd-filing-clock-v1``): the upstream row's ``available_at``, i.e. EDGAR acceptance (FSDS), else ``filed``
+ 46 h; FX-converted rows at the latest H.10 publication used; restatement rows at the restating filing's clock
(``docs/datasets/compustat_std.md``, point-in-time rule).

Acceptance (``accept``): (i) the manifest contract self-check (every listed file re-hashed) and (iv) every part's rows
inside its year (``check_stage``); (ii) ``xsga`` coverage of first-reported rows on the non-financial templates;
(iii) the SG&A convention report (both present; a deterministic 50-issuer FSDS sample, filings stamped 2020 onward
where an issuer has one, classified from face presentation and arithmetic only); and first-reported issuers per
fiscal year. Per owner steering 2026-10-05 the per-year tables run from 2020 through the data end (the stage's max
``available_at``) first, with the request's original windows (2009-2022 / 2009-2023) after them as supporting
evidence. A fiscal year is ``partial`` while one of its fiscal periods is still open at the data end and
``completeness_unverified`` otherwise; there is no filing-deadline model (ruling B3-2).

    python -m atx_db.alpha_panel.compustat_std_parts build
    python -m atx_db.alpha_panel.compustat_std_parts accept
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

from . import common as C

STAGE = "compustat_std_parts"
SCHEMA = "atx.alpha-panel.compustat-std-parts/v1"
RULE = "compustat-std-funda-v1"
CLOCK_RULE = "cstd-filing-clock-v1"
STALENESS_DAYS = 400
MODULES = ("compustat_std_parts", "common")
CONTRACT = {"schema": SCHEMA, "rule": RULE, "clock_rule": CLOCK_RULE, "staleness_days": STALENESS_DAYS}
UPSTREAM_STAGE = "compustat_std"
# compustat_std build 2026-10-03 on fundamentals_v10 (schema v2, rule cstd-pit-v3, git_head 037a73ff)
UPSTREAM_MANIFEST_SHA256 = "9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325"
SOURCE_FILE = "funda.parquet"
PART_FILE = "funda.parquet"
COLUMNS = ("cik", "datadate", "fyear", "fyr", "accession", "available_at", "vintage_kind", "first_reported",
           "xsga", "xrd", "gdwl", "seq", "ceq", "pstk", "txditc", "at", "lt", "fin_template", "curcd", "fx_converted")
NULL_CLOCK_REASON = "available_at IS NULL: the row is never usable (request rule C1)"
MANIFEST_LIMIT = 64 << 20                         # engine kStageManifestLimit
PART_RE = re.compile(r"^parts/year=(\d{4})/[^/]+\.parquet$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
NON_FINANCIAL = ("other", "utility")              # bank, insurer and reit are the financial templates
SEAL = dt.datetime(2024, 1, 1)                    # TRAIN never reads a row stamped at or after this instant
SAMPLE_N = 50
PRIMARY_START = 2020                              # owner steering 2026-10-05: 2020 through the latest data first
TARGET_FROM = dt.datetime(PRIMARY_START, 1, 1)    # SG&A sample: a filing stamped at or after this, where one exists
ORIGINAL_COVERAGE_YEARS = range(2009, 2023)       # the request's (ii) window, kept as supporting evidence
ORIGINAL_HISTORY_YEARS = range(2009, 2024)

RULE_TEXT = {
    "rows": ("every row of the pinned compustat_std funda.parquet (filing and restatement vintages, leavers "
             "included; nothing filtered or overwritten), projected to the 20 requested columns, values unchanged; "
             "NULL available_at excluded and counted"),
    "parts": "parts/year=YYYY/funda.parquet holds exactly the rows with available_at (naive UTC) in [YYYY-01-01, YYYY+1-01-01)",
    "clock": ("available_at of the upstream row: EDGAR acceptance (FSDS), else filed + 46 h; FX-converted rows at the "
              "latest H.10 publication used; restatement rows at the restating filing's clock"),
    "staleness": f"a row describes datadate as known at available_at; consumers treat it as stale {STALENESS_DAYS} days after datadate",
    "history": ("as published upstream: FY2009-2010 are thin (XBRL phase-in, Company Facts / FSDS start 2009Q2); no "
                "comparative-period backfill"),
}


# ---------------------------------------------------------------- build
def _sql_path(path: Path) -> str:
    return path.as_posix().replace("'", "''")


def _verify_upstream(up_dir: Path, pin: str) -> tuple[dict[str, Any], str, dict[str, Any]]:
    mpath = up_dir / "manifest.json"
    raw = mpath.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    if sha != pin.lower():
        raise RuntimeError(f"{mpath}: manifest sha256 {sha} != pin {pin}")
    m = json.loads(raw)
    if m.get("status") != "complete":
        raise RuntimeError(f"{mpath}: status {m.get('status')!r} is not complete")
    ent = (m.get("files") or {}).get(SOURCE_FILE)
    if not ent:
        raise RuntimeError(f"{mpath}: {SOURCE_FILE} is not listed")
    src = up_dir / SOURCE_FILE
    ident = C.file_identity(src)
    if ident["bytes"] != ent["bytes"] or C.sha256_file(src) != ent["sha256"]:
        raise RuntimeError(f"{src}: bytes/sha256 differ from the pinned manifest entry")
    return m, sha, ident


def build(stage: str = STAGE, upstream: str = UPSTREAM_STAGE,
          upstream_manifest_sha256: str = UPSTREAM_MANIFEST_SHA256) -> dict[str, Any]:
    """Write ``<stage>/parts/year=YYYY/funda.parquet`` for every year present, then ``manifest.json`` last."""
    receipt: dict[str, Any] = {}
    root = C.build_root()
    out = root / stage
    if out.exists() and any(out.iterdir()):
        raise RuntimeError(f"{out} already exists: published outputs are never replaced; build a new --stage name")
    up_dir = root / upstream
    with C.timed(receipt, "verify_upstream"):
        up, up_sha, ident = _verify_upstream(up_dir, upstream_manifest_sha256)
    src = _sql_path(up_dir / SOURCE_FILE)
    cols = ", ".join(f'"{c}"' for c in COLUMNS)
    per_part: dict[str, int] = {}
    con = C.connect(memory="400MB", threads=2)
    try:
        have = [r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{src}')").fetchall()]
        missing = [c for c in COLUMNS if c not in have]
        if missing:
            raise RuntimeError(f"{up_dir / SOURCE_FILE}: missing columns {missing}")
        total, nulls = con.execute(f"SELECT count(*), count(*) FILTER (WHERE available_at IS NULL) "
                                   f"FROM read_parquet('{src}')").fetchone()
        years = [int(r[0]) for r in con.execute(f"SELECT DISTINCT year(available_at) FROM read_parquet('{src}') "
                                                "WHERE available_at IS NOT NULL ORDER BY 1").fetchall()]
        with C.timed(receipt, "write_parts"):
            for y in years:
                sql = (f"SELECT {cols} FROM read_parquet('{src}') WHERE available_at >= TIMESTAMP '{y}-01-01' "
                       f"AND available_at < TIMESTAMP '{y + 1}-01-01' ORDER BY cik, datadate, available_at, accession")
                per_part[str(y)] = C.copy_to_parquet(con, sql, out / "parts" / f"year={y}" / PART_FILE)
    finally:
        con.close()
    if C.file_identity(up_dir / SOURCE_FILE) != ident:
        raise RuntimeError(f"{up_dir / SOURCE_FILE}: size or mtime moved while it was read")
    exported = sum(per_part.values())
    if exported + nulls != total:
        raise RuntimeError(f"row conservation: {exported} exported + {nulls} NULL clock != {total} source rows")
    files = {}
    for y in per_part:
        rel = f"parts/year={y}/{PART_FILE}"
        files[rel] = {"bytes": (out / rel).stat().st_size, "sha256": C.sha256_file(out / rel)}
    manifest = {
        "schema": SCHEMA, "status": "complete", "stage": stage, "rule": RULE, "clock_rule": CLOCK_RULE,
        "staleness_days": STALENESS_DAYS, "files": files, "columns": list(COLUMNS),
        "code": C.code_identity(*MODULES),
        "input_manifests_sha256": {f"{upstream}/manifest.json": up_sha},
        "inputs": {f"{upstream}/{SOURCE_FILE}": {"bytes": ident["bytes"], "sha256": up["files"][SOURCE_FILE]["sha256"]}},
        "upstream": {"stage": upstream, "schema": up.get("schema"), "rule": up.get("rule"),
                     "fund_stage": up.get("fund_stage"), "build_date": up.get("build_date"),
                     "gate_status": up.get("gate_status"), "code_git_head": (up.get("code") or {}).get("git_head")},
        "counts": {"source_rows": int(total), "exported_rows": exported, "excluded_null_available_at": int(nulls),
                   "per_part": per_part},
        "excluded": [{"reason": NULL_CLOCK_REASON, "rows": int(nulls)}],
        "rule_text": RULE_TEXT,
        "built_at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        "receipt": receipt,
    }
    C.write_json_atomic(out / "manifest.json", manifest)
    return {"stage": str(out), "manifest_sha256": C.sha256_file(out / "manifest.json"), **manifest["counts"],
            "timings_s": receipt.get("timings_s")}


# ---------------------------------------------------------------- C5 self-check, (i) and (iv)
def _contract_errors(m: Any, contract: dict[str, Any]) -> list[str]:
    if not isinstance(m, dict):
        return ["manifest: not a JSON object"]
    err = []
    for key in ("schema", "rule", "clock_rule"):
        if not isinstance(m.get(key), str) or m[key] != contract[key]:
            err.append(f"{key}: {m.get(key)!r} is not the string {contract[key]!r}")
    if m.get("status") != "complete":
        err.append(f"status: {m.get('status')!r} is not 'complete'")
    s = m.get("staleness_days")
    if type(s) is not int or s != contract["staleness_days"]:
        err.append(f"staleness_days: {s!r} is not the JSON integer {contract['staleness_days']}")
    files = m.get("files")
    if not isinstance(files, dict) or not files:
        return err + ["files: missing, empty or not an object"]
    for rel, ent in files.items():
        if not PART_RE.match(rel) or ".." in rel.split("/"):
            err.append(f"files: {rel!r} is not a parts/year=YYYY/<name>.parquet path")
        if not isinstance(ent, dict) or type(ent.get("bytes")) is not int or ent["bytes"] < 0 \
                or not isinstance(ent.get("sha256"), str) or not SHA_RE.match(ent["sha256"]):
            err.append(f"files: {rel!r} entry {ent!r} is not {{bytes: unsigned int, sha256: lower-case hex}}")
    return err


def check_stage(directory: Path, contract: dict[str, Any], clock: str = "available_at") -> dict[str, Any]:
    """C5 manifest self-check plus the part-year check; ``ok`` only when everything holds."""
    mpath = directory / "manifest.json"
    raw = mpath.read_bytes()
    res: dict[str, Any] = {"manifest": mpath.as_posix(), "manifest_sha256": hashlib.sha256(raw).hexdigest(),
                           "manifest_bytes": len(raw), "files": 0, "verified": 0, "bad": [], "unlisted": [],
                           "parts": {}, "rows_outside_year": 0}
    m = json.loads(raw) if len(raw) <= MANIFEST_LIMIT else None
    res["contract_errors"] = (["manifest: larger than 64 MiB"] if m is None else []) + _contract_errors(m, contract)
    files = m.get("files") if isinstance(m, dict) and isinstance(m.get("files"), dict) else {}
    res["files"] = len(files)
    good = []
    for rel, ent in sorted(files.items()):
        if not PART_RE.fullmatch(rel) or ".." in rel.split("/"):
            res["bad"].append({"rel": rel, "why": "invalid part path"})
            continue
        p = directory / rel
        if not isinstance(ent, dict) or not p.is_file():
            res["bad"].append({"rel": rel, "why": "missing"})
        elif p.stat().st_size != ent.get("bytes"):
            res["bad"].append({"rel": rel, "why": f"bytes {p.stat().st_size} != {ent.get('bytes')}"})
        elif C.sha256_file(p) != ent.get("sha256"):
            res["bad"].append({"rel": rel, "why": "sha256 differs"})
        else:
            res["verified"] += 1
            good.append(rel)
    on_disk = {p.relative_to(directory).as_posix() for p in (directory / "parts").rglob("*") if p.is_file()} \
        if (directory / "parts").exists() else set()
    res["unlisted"] = sorted(on_disk - set(files))
    con = C.connect(memory="300MB", threads=1)
    try:
        for rel in good:
            mt = PART_RE.match(rel)
            if not mt:
                continue
            y = int(mt.group(1))
            n, lo, hi, out = con.execute(
                f"SELECT count(*), min({clock}), max({clock}), count(*) FILTER (WHERE {clock} IS NULL "
                f"OR {clock} < TIMESTAMP '{y}-01-01' OR {clock} >= TIMESTAMP '{y + 1}-01-01') "
                f"FROM read_parquet('{_sql_path(directory / rel)}')").fetchone()
            res["parts"][rel] = {"rows": int(n), "min": str(lo), "max": str(hi), "outside_year": int(out)}
            res["rows_outside_year"] += int(out)
    finally:
        con.close()
    res["ok"] = (not res["contract_errors"] and not res["bad"] and not res["unlisted"] and res["files"] > 0
                 and res["verified"] == res["files"] and res["rows_outside_year"] == 0)
    return res


# ---------------------------------------------------------------- (ii), history, (iii)
def _parts(stage_dir: Path) -> str:
    m = C.read_json(stage_dir / "manifest.json")
    paths = ", ".join(f"'{_sql_path(stage_dir / rel)}'" for rel in sorted(m["files"]))
    return f"read_parquet([{paths}])"


def data_end(con, stage_dir: Path) -> dt.datetime:
    """The source's max ``available_at`` over the listed parts (the end of the observed data)."""
    return con.execute(f"SELECT max(available_at) FROM {_parts(stage_dir)}").fetchone()[0]


def fiscal_year_status(fyear: int, end: dt.datetime) -> str:
    """``partial`` while a fiscal period of the year (Compustat fyear: FYE June Y .. May Y+1; a 52/53-week year can end
    in the first days of June Y+1, observed up to 2023-06-03 for fyear 2022) is still open at ``end``; otherwise
    ``completeness_unverified``: rows are as observed at ``end`` and no source evidence closes any fiscal year (late
    filings and restatement vintages can still add rows). No filing-deadline model (ruling B3-2)."""
    return "partial" if end < dt.datetime(fyear + 1, 6, 8) else "completeness_unverified"


def fiscal_years_observed_sql(stage_dir: Path, start: int) -> str:
    return (f"SELECT fyear, min(datadate), max(datadate), count(*), count(DISTINCT cik), max(available_at) "
            f"FROM {_parts(stage_dir)} WHERE first_reported AND fyear >= {int(start)} GROUP BY 1 ORDER BY 1")


def xsga_coverage_sql(stage_dir: Path, years: range) -> str:
    return (f"SELECT fyear, count(*), count(xsga), count(*) FILTER (WHERE available_at < TIMESTAMP '{SEAL:%Y-%m-%d}'), "
            f"count(xsga) FILTER (WHERE available_at < TIMESTAMP '{SEAL:%Y-%m-%d}') FROM {_parts(stage_dir)} "
            f"WHERE first_reported AND fin_template IN {NON_FINANCIAL!r} AND fyear BETWEEN {years[0]} AND {years[-1]} "
            "GROUP BY 1 ORDER BY 1")


def xsga_coverage(con, stage_dir: Path, years: range = range(2009, 2023)) -> dict[str, Any]:
    """(ii) Per fiscal year: first-reported non-financial rows and those with ``xsga`` (all, and stamped < 2024)."""
    sql = xsga_coverage_sql(stage_dir, years)
    got = {int(r[0]): r[1:] for r in con.execute(sql).fetchall()}
    keys = ("rows", "with_xsga", "rows_pre2024", "with_xsga_pre2024")
    by = {y: dict(zip(keys, (int(v) for v in got.get(y, (0, 0, 0, 0))))) for y in years}
    total = {k: sum(v[k] for v in by.values()) for k in keys}
    return {"sql": sql, "by_year": by, "total": total}


def first_reported_issuers_sql(stage_dir: Path, years: range) -> str:
    return (f"SELECT fyear, count(DISTINCT cik), count(DISTINCT cik) FILTER (WHERE available_at < "
            f"TIMESTAMP '{SEAL:%Y-%m-%d}') FROM {_parts(stage_dir)} WHERE first_reported "
            f"AND fyear BETWEEN {years[0]} AND {years[-1]} GROUP BY 1 ORDER BY 1")


def first_reported_issuers(con, stage_dir: Path, years: range = range(2009, 2024)) -> dict[int, dict[str, int]]:
    """Per fiscal year: issuers with a first-reported row (all templates), all and stamped < 2024."""
    got = {int(r[0]): r[1:] for r in con.execute(first_reported_issuers_sql(stage_dir, years)).fetchall()}
    return {y: {"issuers": int(got.get(y, (0, 0))[0]), "issuers_pre2024": int(got.get(y, (0, 0))[1])} for y in years}


def both_present_sql(stage_dir: Path) -> str:
    return (f"SELECT count(*), count(*) FILTER (WHERE xsga IS NOT NULL AND xrd IS NOT NULL), "
            f"count(*) FILTER (WHERE xsga IS NOT NULL AND xrd > 0), count(*) FILTER (WHERE first_reported), "
            f"count(*) FILTER (WHERE first_reported AND xsga IS NOT NULL AND xrd IS NOT NULL), "
            f"count(*) FILTER (WHERE first_reported AND xsga IS NOT NULL AND xrd > 0) FROM {_parts(stage_dir)}")


def both_present(con, stage_dir: Path) -> dict[str, Any]:
    """(iii) Rows with both ``xsga`` and ``xrd`` non-NULL (a zero-filled ``xrd`` counts), and with ``xrd`` > 0."""
    sql = both_present_sql(stage_dir)
    r = [int(v) for v in con.execute(sql).fetchone()]
    return {"sql": sql, "rows": r[0], "both_present": r[1], "xsga_and_xrd_positive": r[2], "first_reported_rows": r[3],
            "first_reported_both_present": r[4], "first_reported_xsga_and_xrd_positive": r[5]}


# SG&A convention: face presentation (FSDS ``pre``, income statement) and arithmetic (FSDS ``num``) only. FSDS has no
# calculation linkbase, so a filing is decided only when its face says so; everything else is ``unresolved``.
SGA_TAGS = ("SellingGeneralAndAdministrativeExpense", "OtherSellingGeneralAndAdministrativeExpense")
SGA_FALLBACK_TAGS = ("GeneralAndAdministrativeExpense", "SellingAndMarketingExpense", "SellingExpense")
RD_TAGS = ("ResearchAndDevelopmentExpense", "ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost",
           "ResearchAndDevelopmentExpenseSoftwareExcludingAcquiredInProcessCost")
TOTAL_TAGS = ("OperatingExpenses", "CostsAndExpenses", "OperatingCostsAndExpenses")
OI_TAGS = ("OperatingIncomeLoss",)
RD_LABEL = re.compile(r"research|\br\s*&\s*d\b", re.IGNORECASE)


def _total_identity(lines, total, need, rd_value, rd_below_sga, tol) -> tuple[str, str] | None:
    """Expense total: the contiguous block of face lines just above it sums to it."""
    acc, seen = 0.0, set()
    for x in reversed([x for x in lines if x["line"] < total["line"] and x["value"] is not None]):
        acc += x["value"]
        seen.add(x["line"])
        if not need <= seen:
            continue
        if abs(acc - total["value"]) <= tol:
            return "excludes_rd", "face_identity_total_equals_sga_plus_rd"
        if rd_below_sga and abs(acc - rd_value - total["value"]) <= tol:
            return "includes_rd", "face_identity_total_counts_rd_inside_sga"
    return None


def _oi_identity(lines, oi, need, rd_value, rd_below_sga, rel_tol) -> tuple[str, str] | None:
    """Operating income: a face line above it (gross profit, revenue) minus the contiguous block between equals it."""
    before = [x for x in lines if x["line"] < oi["line"] and x["value"] is not None]
    acc, seen = 0.0, set()
    for anchor in reversed(before):
        if need <= seen:
            tol = max(max(abs(anchor["value"]), abs(oi["value"])) * rel_tol, 1.0)
            if abs(rd_value) > 2 * tol:
                if abs(anchor["value"] - acc - oi["value"]) <= tol:
                    return "excludes_rd", "face_identity_anchor_minus_sga_and_rd_equals_operating_income"
                if rd_below_sga and abs(anchor["value"] - (acc - rd_value) - oi["value"]) <= tol:
                    return "includes_rd", "face_identity_operating_income_counts_rd_inside_sga"
        acc += anchor["value"]
        seen.add(anchor["line"])
    return None


def classify_sga(lines: list[dict[str, Any]], rel_tol: float = 5e-4) -> tuple[str, str]:
    """(class, basis) for one filing's income-statement face lines ``{line, tag, value, plabel}``.

    ``includes_rd``: an SG&A face label explicitly includes R&D, or an R&D line directly below SG&A is not additive (the face
    reconciles only without it). ``excludes_rd``: SG&A and R&D are both additive in a face identity: the contiguous
    block above an expense total sums to it, or a line above operating income minus the block between equals it.
    Anything else is ``unresolved`` with the reason; nothing is inferred from the numeric fields alone.
    """
    if not lines:
        return "unresolved", "no_fsds_income_statement"
    lines = sorted(lines, key=lambda x: x["line"])
    sga = [x for x in lines if x["tag"] in SGA_TAGS] or [x for x in lines if x["tag"] in SGA_FALLBACK_TAGS]
    if not sga:
        return "unresolved", "sga_not_on_face"
    labels = [x.get("plabel") or "" for x in sga if RD_LABEL.search(x.get("plabel") or "")]
    if labels:
        decisions = set()
        for label in labels:
            if re.search(r"\b(exclud(?:e|es|ing)|without|not\s+includ(?:e|ed|ing))\b.{0,50}(research|r\s*&\s*d)", label, re.I):
                decisions.add("excludes_rd")
            elif re.search(r"\b(includ(?:e|es|ed|ing))\b.{0,50}(research|r\s*&\s*d)", label, re.I):
                decisions.add("includes_rd")
            else:
                decisions.add("unresolved")
        if len(decisions) == 1 and "unresolved" not in decisions:
            cls = decisions.pop()
            return cls, "sga_face_label_explicit_" + cls
        return "unresolved", "sga_face_label_ambiguous_rd"
    rd = [x for x in lines if x["tag"] in RD_TAGS]
    if not rd:
        return "unresolved", "rd_not_on_face"
    last = max(x["line"] for x in sga + rd)
    total = next((x for x in lines if x["tag"] in TOTAL_TAGS and x["line"] > last), None)
    oi = next((x for x in lines if x["tag"] in OI_TAGS and x["line"] > last), None)
    if total is None and oi is None:
        return "unresolved", "no_total_line_after_sga_and_rd"
    targets = [t for t in (total, oi) if t is not None and t["value"] is not None]
    if not targets or any(x["value"] is None for x in sga + rd):
        return "unresolved", "face_values_missing"
    rd_value = sum(x["value"] for x in rd)
    if abs(rd_value) <= 2 * max(max(abs(t["value"]) for t in targets) * rel_tol, 1.0):
        return "unresolved", "rd_within_tolerance"
    sga_lines = {x["line"] for x in sga}
    rd_below_sga = all(x["line"] - 1 in sga_lines for x in rd)
    need = sga_lines | {x["line"] for x in rd}
    got = None
    if total is not None and total["value"] is not None:
        got = _total_identity(lines, total, need, rd_value, rd_below_sga, max(abs(total["value"]) * rel_tol, 1.0))
    if got is None and oi is not None and oi["value"] is not None:
        got = _oi_identity(lines, oi, need, rd_value, rd_below_sga, rel_tol)
    return got or ("unresolved", "face_identity_not_found")


def sga_sample_sql(stage_dir: Path, n: int, fsds: Path = C.FSDS_DIR) -> str:
    """Deterministic sample of R&D-reporting issuers (a first-reported row with ``xrd`` > 0 and ``xsga`` present).

    Per issuer one filing: a target-period one (stamped on or after ``TARGET_FROM``) where the issuer has one, then
    one present in the local FSDS, then the latest. Issuers with a target-period filing come first, each group
    ordered by md5 of the CIK."""
    return (f"WITH fs AS (SELECT DISTINCT adsh FROM read_parquet('{_sql_path(fsds / 'sub' / '*.parquet')}')), "
            f"r AS (SELECT cik, accession, datadate, fyear, available_at, xsga, xrd, curcd, fx_converted, "
            f"available_at >= TIMESTAMP '{TARGET_FROM:%Y-%m-%d}' AS target, fs.adsh IS NOT NULL AS in_fsds "
            f"FROM {_parts(stage_dir)} LEFT JOIN fs ON fs.adsh = accession "
            "WHERE first_reported AND xrd > 0 AND xsga IS NOT NULL), "
            "pool AS (SELECT * FROM r QUALIFY row_number() OVER (PARTITION BY cik ORDER BY target DESC, in_fsds DESC, "
            "available_at DESC, accession DESC) = 1) "
            "SELECT cik, accession, datadate, fyear, available_at, xsga, xrd, target, in_fsds, "
            "(SELECT count(*) FROM pool) AS pool_issuers, (SELECT count(*) FROM pool WHERE target) AS pool_target, curcd, fx_converted "
            f"FROM pool ORDER BY target DESC, md5(CAST(cik AS VARCHAR)), cik LIMIT {int(n)}")


def _fsds_face(con, fsds: Path, accessions: list[str]) -> dict[str, list[dict[str, Any]]]:
    """Per accession: the income-statement face lines (``stmt`` IS, not parenthetical) with the current-year value."""
    if not accessions:
        return {}
    acc = ", ".join(f"'{a}'" for a in accessions)
    quarters = [r[0] for r in con.execute(
        f"SELECT DISTINCT quarter FROM read_parquet('{_sql_path(fsds / 'sub' / '*.parquet')}') WHERE adsh IN ({acc})"
    ).fetchall()]
    quarters = [q for q in sorted(quarters) if (fsds / "pre" / f"{q}.parquet").is_file()
                and (fsds / "num" / f"{q}.parquet").is_file()]
    if not quarters:
        return {}
    pre = ", ".join(f"'{_sql_path(fsds / 'pre' / f'{q}.parquet')}'" for q in quarters)
    num = ", ".join(f"'{_sql_path(fsds / 'num' / f'{q}.parquet')}'" for q in quarters)
    rows = con.execute(f"""
        WITH p AS (SELECT adsh, report, line, tag, version, plabel FROM read_parquet([{pre}])
                   WHERE adsh IN ({acc}) AND stmt = 'IS' AND NOT coalesce(inpth, false)),
             pick AS (SELECT adsh, report FROM p GROUP BY adsh, report
                      QUALIFY row_number() OVER (PARTITION BY adsh ORDER BY
                          count(*) FILTER (WHERE tag IN {SGA_TAGS + SGA_FALLBACK_TAGS + RD_TAGS + TOTAL_TAGS!r}) DESC,
                          report) = 1),
             n0 AS (SELECT adsh, tag, version, ddate, uom, CAST(value AS DOUBLE) AS value FROM read_parquet([{num}])
                    WHERE adsh IN ({acc}) AND qtrs = 4 AND coalesce(segments, '') = '' AND coalesce(coreg, '') = ''),
             face AS (SELECT p.* FROM p JOIN pick USING (adsh, report)),
             unit AS (SELECT n0.adsh, n0.uom FROM n0 JOIN face USING (adsh, tag, version) GROUP BY 1, 2
                      QUALIFY row_number() OVER (PARTITION BY n0.adsh ORDER BY count(*) DESC, n0.uom) = 1),
             n AS (SELECT n0.* FROM n0 JOIN unit USING (adsh, uom)),
             cur AS (SELECT n.adsh, max(n.ddate) AS ddate FROM n JOIN face f USING (adsh, tag, version) GROUP BY 1),
             v AS (SELECT n.adsh, n.tag, n.version, any_value(n.uom) AS uom,
                          CASE WHEN count(DISTINCT n.value) = 1 THEN max(n.value) END AS value
                   FROM n JOIN cur USING (adsh, ddate) GROUP BY 1, 2, 3)
        SELECT f.adsh, f.line, f.tag, f.plabel, v.value, v.uom FROM face f LEFT JOIN v USING (adsh, tag, version)
        ORDER BY f.adsh, f.line""").fetchall()
    out: dict[str, list[dict[str, Any]]] = {}
    for a, line, tag, plabel, value, uom in rows:
        out.setdefault(a, []).append({"line": int(line), "tag": tag, "plabel": plabel, "value": value, "uom": uom})
    return out


def sga_convention_sample(con, stage_dir: Path, fsds: Path = C.FSDS_DIR, n: int = SAMPLE_N) -> dict[str, Any]:
    """(iii) Whether the filed SG&A element includes R&D, for ``n`` sampled R&D-reporting issuers (report only)."""
    sql = sga_sample_sql(stage_dir, n, fsds)
    picked = con.execute(sql).fetchall()
    faces = _fsds_face(con, fsds, [r[1] for r in picked])
    issuers = []
    for cik, accession, datadate, fyear, available_at, xsga, xrd, target, in_fsds, _pool, _target, curcd, fx_converted in picked:
        lines = faces.get(accession, [])
        cls, basis = classify_sga(lines)
        face_sga = [x["value"] for x in lines if x["tag"] in SGA_TAGS] or \
            [x["value"] for x in lines if x["tag"] in SGA_FALLBACK_TAGS]
        face_sga_v = None if not face_sga or None in face_sga else sum(face_sga)
        units = {x.get("uom") for x in lines if x["tag"] in SGA_TAGS + SGA_FALLBACK_TAGS}
        comparison_basis = ("inapplicable_fx_converted" if fx_converted else
                            "inapplicable_units_unknown_or_differ" if units != {curcd} or not curcd else
                            "inapplicable_face_value_missing" if face_sga_v is None else "same_currency")
        issuers.append({"cik": int(cik), "accession": accession, "datadate": str(datadate), "fyear": int(fyear),
                        "available_at": str(available_at), "target": bool(target), "in_fsds": bool(in_fsds),
                        "xsga": xsga, "xrd": xrd, "class": cls, "basis": basis, "face_lines": len(lines),
                        "uom": next((x["uom"] for x in lines if x.get("uom")), None), "face_sga": face_sga_v,
                        "curcd": curcd, "fx_converted": bool(fx_converted), "comparison_basis": comparison_basis,
                        "xsga_equals_face_sga": (abs(face_sga_v - xsga) <= max(abs(xsga) * 5e-3, 1.0))
                        if comparison_basis == "same_currency" else None})
    counts: dict[str, int] = {}
    for i in issuers:
        key = i["class"] if i["class"] != "unresolved" else f"unresolved:{i['basis']}"
        counts[key] = counts.get(key, 0) + 1
    return {"sql": sql, "pool_issuers": int(picked[0][9]) if picked else 0,
            "pool_target_issuers": int(picked[0][10]) if picked else 0, "sampled": len(issuers),
            "sampled_target_filings": sum(i["target"] for i in issuers), "counts": dict(sorted(counts.items())),
            "issuers": issuers}


# ---------------------------------------------------------------- accept
def _share(a: int, b: int) -> str:
    return f"{a / b:.4f}" if b else "n/a"


def accept(stage: str = STAGE, fsds: Path = C.FSDS_DIR, n: int = SAMPLE_N) -> tuple[bool, list[str]]:
    """The B3 acceptance prints; ``ok`` is the (i)+(iv) self-check (the other prints are reports)."""
    d = C.build_root() / stage
    out = [f"$ python -m atx_db.alpha_panel.compustat_std_parts accept --stage {stage}"]
    chk = check_stage(d, CONTRACT)
    manifest = C.read_json(d / "manifest.json")
    out += [f"  upstream manifest pins: {manifest.get('input_manifests_sha256')}",
            f"  upstream gate_status (inherited, not repaired by export): {manifest.get('upstream', {}).get('gate_status')}",
            f"  export counts: {manifest.get('counts')}"]
    out += ["", "(i) C5 manifest self-check (engine PinnedStage contract; every listed file re-hashed)",
            f"  manifest {chk['manifest']} bytes={chk['manifest_bytes']} sha256={chk['manifest_sha256']}",
            f"  contract errors={len(chk['contract_errors'])} {chk['contract_errors']}",
            f"  files listed={chk['files']} verified={chk['verified']} bad={len(chk['bad'])} {chk['bad']} "
            f"unlisted={len(chk['unlisted'])} {chk['unlisted']}",
            "", "(iv) part year check: count(*) FILTER (WHERE available_at IS NULL OR available_at < 'Y-01-01' "
            "OR available_at >= 'Y+1-01-01') per parts/year=Y"]
    for rel, p in chk["parts"].items():
        out.append(f"  {rel} rows={p['rows']} min={p['min']} max={p['max']} outside_year={p['outside_year']}")
    out.append(f"  rows_outside_year={chk['rows_outside_year']} rows_total={sum(p['rows'] for p in chk['parts'].values())}")
    out.append(f"  (i)+(iv) {'PASS' if chk['ok'] else 'FAIL'}")
    if not chk["ok"]:
        return False, out
    con = C.connect(memory="400MB", threads=2)
    try:
        end = data_end(con, d)
        prim = range(PRIMARY_START, end.year + 1)
        out += ["", f"data end (max available_at) {end}; availability year {end.year} is partial (rows through {end}). "
                f"Primary window {prim[0]}-{prim[-1]} (owner steering 2026-10-05); the request's original windows "
                "follow each primary table as supporting evidence.",
                "fiscal-year status: partial = a fiscal period of the year (FYE June Y .. early June Y+1) is still open "
                "at the data end;completeness_unverified = rows as observed at the data end, no source evidence closes the "
                "year (late filings and restatement vintages can still add rows)",
                "", f"observed fiscal years {prim[0]}+ (first-reported rows, all templates)",
                f"  sql: {fiscal_years_observed_sql(d, prim[0])}",
                "  fyear status min_datadate max_datadate rows issuers max_available_at"]
        for fy, lo, hi, rows, iss, last in con.execute(fiscal_years_observed_sql(d, prim[0])).fetchall():
            out.append(f"  {fy} {fiscal_year_status(int(fy), end)} {lo} {hi} {rows} {iss} {last}")
        cov = xsga_coverage(con, d, prim)
        out += ["", f"(ii) xsga coverage, primary window {prim[0]}-{prim[-1]}: first-reported rows, fin_template in "
                f"{NON_FINANCIAL!r}, every vintage stamp", f"  sql: {cov['sql']}", "  fyear status rows with_xsga share"]
        for y, v in cov["by_year"].items():
            out.append(f"  {y} {fiscal_year_status(y, end)} {v['rows']} {v['with_xsga']} {_share(v['with_xsga'], v['rows'])}")
        out.append(f"  total {cov['total']['rows']} {cov['total']['with_xsga']} {_share(cov['total']['with_xsga'], cov['total']['rows'])}")
        cov = xsga_coverage(con, d, ORIGINAL_COVERAGE_YEARS)
        out += ["", f"(ii) xsga coverage, original window {ORIGINAL_COVERAGE_YEARS[0]}-{ORIGINAL_COVERAGE_YEARS[-1]} "
                f"(supporting): first-reported rows, fin_template in {NON_FINANCIAL!r}",
                f"  sql: {cov['sql']}", "  fyear rows with_xsga share | stamped<2024: rows with_xsga share"]
        for y, v in list(cov["by_year"].items()) + [("total", cov["total"])]:
            out.append(f"  {y} {v['rows']} {v['with_xsga']} {_share(v['with_xsga'], v['rows'])} | "
                       f"{v['rows_pre2024']} {v['with_xsga_pre2024']} {_share(v['with_xsga_pre2024'], v['rows_pre2024'])}")
        both = both_present(con, d)
        out += ["", "(iii) SG&A convention (report, not a gate)", f"  sql: {both['sql']}",
                f"  all rows={both['rows']} xsga&xrd non-NULL={both['both_present']} "
                f"share={_share(both['both_present'], both['rows'])}; xsga non-NULL & xrd>0={both['xsga_and_xrd_positive']} "
                f"share={_share(both['xsga_and_xrd_positive'], both['rows'])}",
                f"  first-reported rows={both['first_reported_rows']} xsga&xrd non-NULL={both['first_reported_both_present']} "
                f"share={_share(both['first_reported_both_present'], both['first_reported_rows'])}; "
                f"xsga non-NULL & xrd>0={both['first_reported_xsga_and_xrd_positive']} "
                f"share={_share(both['first_reported_xsga_and_xrd_positive'], both['first_reported_rows'])}"]
        smp = sga_convention_sample(con, d, fsds, n)
        out += [f"  sample sql: {smp['sql']}",
                f"  FSDS {fsds.as_posix()} pre (stmt IS, inpth false, the IS report with most SG&A/R&D/total tags) + "
                "num (qtrs 4, no segments/coreg, the filing's dominant unit, latest ddate)",
                f"  pool issuers={smp['pool_issuers']} with a filing stamped >= {TARGET_FROM:%Y-%m-%d}="
                f"{smp['pool_target_issuers']} sampled={smp['sampled']} sampled target-period filings="
                f"{smp['sampled_target_filings']} classes={smp['counts']}",
                "  cik accession fyear available_at in_fsds class basis face_lines uom xsga face_sga xsga_equals_face_sga xrd curcd fx_converted comparison_basis"]
        for i in smp["issuers"]:
            out.append(f"  {i['cik']} {i['accession']} {i['fyear']} {i['available_at']} {i['in_fsds']} {i['class']} "
                       f"{i['basis']} {i['face_lines']} {i['uom']} {i['xsga']} {i['face_sga']} "
                       f"{i['xsga_equals_face_sga']} {i['xrd']} {i['curcd']} {i['fx_converted']} {i['comparison_basis']}")
        hist = first_reported_issuers(con, d, prim)
        out += ["", f"history: issuers with a first-reported row per fiscal year, primary window {prim[0]}-{prim[-1]} "
                "(all templates, every vintage stamp)", f"  sql: {first_reported_issuers_sql(d, prim)}",
                "  fyear status issuers"]
        out += [f"  {y} {fiscal_year_status(y, end)} {v['issuers']}" for y, v in hist.items()]
        hist = first_reported_issuers(con, d, ORIGINAL_HISTORY_YEARS)
        out += ["", f"history: issuers with a first-reported row per fiscal year, original window "
                f"{ORIGINAL_HISTORY_YEARS[0]}-{ORIGINAL_HISTORY_YEARS[-1]} (supporting; all templates)",
                f"  sql: {first_reported_issuers_sql(d, ORIGINAL_HISTORY_YEARS)}", "  fyear issuers stamped<2024"]
        out += [f"  {y} {v['issuers']} {v['issuers_pre2024']}" for y, v in hist.items()]
    finally:
        con.close()
    return chk["ok"], out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--upstream", default=UPSTREAM_STAGE)
    b.add_argument("--upstream-manifest-sha256", default=UPSTREAM_MANIFEST_SHA256)
    a = sub.add_parser("accept")
    a.add_argument("--sample", type=int, default=SAMPLE_N)
    for p in (b, a):
        p.add_argument("--stage", default=STAGE)
    args = ap.parse_args(argv)
    if args.cmd == "build":
        print(json.dumps(build(args.stage, args.upstream, args.upstream_manifest_sha256), indent=2, default=str))
        return 0
    ok, lines = accept(args.stage, n=args.sample)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
