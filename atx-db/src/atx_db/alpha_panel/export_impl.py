"""Stage E: export a window of the alpha panel in the atx-impl binary role/fields layout.

Writes ``<out>/role`` (``atx.recent-research-role/v1``: sessions.i64, ids.u64, present.u8,
member.u8, close.f64, raw_close.f64, volume.f64, manifest.json) and ``<out>/fields``
(``atx.research-role-fields/v1``: one date-major little-endian f64 file per field). These are
the contracts read by ``atx-engine/src/data/strategy_data.cpp`` and
``atx-impl/src/strategy_ic_runner.cpp``.

The consumer's TRAIN window ends 2023-12-31 (binding OD-1): nothing on or after 2024-01-01 is delivered, so a
loadable export ends by 2023-12-31. Later sessions stay available in the Parquet panel.

``close`` is the panel's total-return close (vendor daily total return chained, factor-break-v1
repaired); the loader's fixed ``close_basis`` string is kept for compatibility and the actual
basis is recorded in ``close_basis_note``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

import numpy as np

from . import common as C
from . import char_registry as R
from .characteristics import panel_stage
from .gold import feature_quality_flags

ROLE_SCHEMA = "atx.recent-research-role/v1"
FIELDS_SCHEMA = "atx.research-role-fields/v1"
SEAL = dt.date(2024, 1, 1)
WARMUP_SESSIONS = 400
MEMBERSHIP = {
    "common_stock_verified": False, "lag_sessions": 1, "lookback_sessions": 63, "min_adv_exclusive": 5000000,
    "min_raw_price_exclusive": 5, "missing": "complete-prior-calendar-window-required",
    "rule": "research-prior63-usd-adv-topn-v1", "ties": "securityID-ascending", "top_n": 3000,
}
EPOCH = dt.date(1970, 1, 1)
NS_DAY = 86_400_000_000_000

FUND_CLOCK = ("issuer-latest-filing-clock<22:00UTC(session-1);FSDS-accepted_utc-else-filed+46h;"
              "P-line-only;stale>400d-from-period_end")
FIELD_SPECS: dict[str, dict[str, Any]] = {
    "mkt_ret": {"units": "simple return", "clock": "role-close-mark", "src": "panel.mkt_ret",
                "definition": "equal-weight mean of non-guarded ret over prior-session members present at d-1 and d"},
    "shares_out": {"units": "shares", "clock": "A8-vendor-shares-lag90-restated", "src": "panel.shares_out"},
    "me_company": {"units": "USD", "clock": "shares_out x raw_close summed over the issuer's linked lines",
                   "src": "panel.me_company"},
    "si_shares": {"units": "shares short", "clock": "finra dissemination_date < session; age<=45d", "src": "panel.si_shares"},
    "si_dtc": {"units": "days to cover (FINRA floored at 1)", "clock": "finra dissemination_date < session; age<=45d",
               "src": "panel.si_dtc"},
    "iv_atm_21d": {"units": "annualized decimal", "clock": "vendor-eod-same-date", "src": "panel.iv_atm_21d"},
    "iv_atm_63d": {"units": "annualized decimal", "clock": "vendor-eod-same-date", "src": "panel.iv_atm_63d"},
    "iv_atm_126d": {"units": "annualized decimal", "clock": "vendor-eod-same-date", "src": "panel.iv_atm_126d"},
    "iv_atm_252d": {"units": "annualized decimal", "clock": "vendor-eod-same-date", "src": "panel.iv_atm_252d"},
    "earn_recent": {"units": "indicator", "src": "panel.earn_recent",
                    "clock": "SEC 8-K Item 2.02 reaction session of the linked issuer and the session after it; the "
                             "announcement is known by the reaction session's 22:00 UTC mark (vendor earnFlag unused)"},
    "sue": {"units": "standardized", "clock": FUND_CLOCK.replace("400d", "200d"), "src": "panel.sue"},
    "grp_sic2": {"units": "group code", "clock": "issuer SIC as of filing clock; stale>550d", "src": "panel.grp_sic2"},
    "grp_ff12": {"units": "group code", "clock": "issuer SIC as of filing clock; stale>550d", "src": "panel.grp_ff12"},
    "grp_ff49": {"units": "group code", "clock": "issuer SIC as of filing clock; stale>550d", "src": "panel.grp_ff49"},
}
for _item in ("at", "at_lag4", "lt", "che", "debt", "be", "be_lag1q", "be_lag1q_lag4", "sale_ttm", "sale_q", "sale_q_lag4",
              "cogs_ttm", "xsga_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4", "cfo_ttm", "capx_ttm",
              "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q", "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa",
              "noa_lag4", "fscore"):
    FIELD_SPECS[_item] = {"units": "USD or shares (issuer filing)", "clock": FUND_CLOCK, "src": f"panel.{_item}"}
DEFAULT_FIELDS = tuple(FIELD_SPECS)


def _sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def _write(path: Path, arr: np.ndarray) -> dict[str, Any]:
    tmp = path.with_name(path.name + ".partial")
    arr.tofile(tmp)
    os.replace(tmp, path)
    return {"bytes": path.stat().st_size, "sha256": _sha(path)}


def _code_map(values: list[Any]) -> dict[Any, float]:
    """Stable numeric codes for group labels (numbers keep their value)."""
    out: dict[Any, float] = {}
    labels = sorted({v for v in values if v is not None}, key=lambda v: str(v))
    for i, v in enumerate(labels):
        try:
            out[v] = float(v)
        except (TypeError, ValueError):
            out[v] = float(i + 1)
    return out


ISSUER_ITEMS = {k for k, v in FIELD_SPECS.items() if v["clock"].startswith("issuer-latest")} | {"sue"}


def export(out: Path, score_start: dt.date, end: dt.date, fields: list[str], issuer_lines: str = "primary") -> dict[str, Any]:
    if end >= SEAL:
        raise SystemExit(f"end {end} is on/after the atx-engine role seal {SEAL}; the loader would refuse it")
    con = C.connect(memory="500MB", threads=2)
    cal = [d for d in C.load_calendar(con) if d <= end]
    sb = next(i for i, d in enumerate(cal) if d >= score_start)
    first = max(0, sb - WARMUP_SESSIONS)
    sessions = cal[first:]
    score_begin = sb - first
    if score_begin < 383:
        raise SystemExit("need >= 383 warm-up sessions before the score start")
    d = len(sessions)
    panel = (C.build_root() / panel_stage() / "*" / "*.parquet").as_posix()
    lo, hi = sessions[0], sessions[-1]
    ids = [r[0] for r in con.execute(f"""
        SELECT DISTINCT security_id FROM read_parquet('{panel}', hive_partitioning = false)
        WHERE session_date BETWEEN DATE '{lo}' AND DATE '{hi}' AND member AND security_id > 0 ORDER BY 1""").fetchall()]
    n = len(ids)
    ids_arr = np.array(ids, dtype=np.int64)
    sess_days = np.array([(s - EPOCH).days for s in sessions], dtype=np.int64)
    role_dir, fields_dir = out / "role", out / "fields"
    role_dir.mkdir(parents=True, exist_ok=False)
    fields_dir.mkdir(parents=True, exist_ok=False)
    con.execute("CREATE TEMP TABLE ax AS SELECT unnest(?::BIGINT[]) AS security_id", [ids])

    def matrix(expr: str, dtype: str = "<f8", fill: float = np.nan, where: str = "TRUE") -> np.ndarray:
        m = np.full((d, n), fill, dtype=dtype)
        for y in range(lo.year, hi.year + 1):
            rows = con.execute(f"""
                SELECT date_diff('day', DATE '1970-01-01', p.session_date) AS dday, p.security_id, {expr} AS v
                FROM read_parquet('{(C.build_root() / panel_stage() / f'year={y}' / '*.parquet').as_posix()}', hive_partitioning = false) p
                JOIN ax USING (security_id)
                WHERE p.session_date BETWEEN DATE '{lo}' AND DATE '{hi}' AND ({where})""").fetchnumpy()
            if not len(rows["v"]):
                continue
            dd = np.asarray(rows["dday"], dtype=np.int64)
            ss = np.asarray(rows["security_id"], dtype=np.int64)
            ri = np.searchsorted(sess_days, dd)
            ci = np.searchsorted(ids_arr, ss)
            if not (np.array_equal(sess_days[ri], dd) and np.array_equal(ids_arr[ci], ss)):
                raise RuntimeError("panel row outside the export axes")
            v = rows["v"]
            if hasattr(v, "filled"):
                v = v.filled(fill)
            m[ri, ci] = v.astype(dtype)
        return m

    valid = "close > 0 AND isfinite(close) AND raw_close > 0 AND isfinite(raw_close) AND volume >= 0 AND isfinite(volume)"
    present = matrix("1", "u1", 0, valid)
    member = matrix("CASE WHEN member THEN 1 ELSE 0 END", "u1", 0, valid)
    member[:63] = 0
    files: dict[str, Any] = {}
    sess_ns = np.array([(s - EPOCH).days * NS_DAY for s in sessions], dtype="<i8")
    files["sessions.i64"] = _write(role_dir / "sessions.i64", sess_ns)
    files["ids.u64"] = _write(role_dir / "ids.u64", np.array(ids, dtype="<u8"))
    files["present.u8"] = _write(role_dir / "present.u8", present)
    files["member.u8"] = _write(role_dir / "member.u8", member)
    for name, expr in (("close", "close"), ("raw_close", "raw_close"), ("volume", "volume")):
        m = matrix(expr, "<f8", np.nan, valid)
        m[present == 0] = np.nan
        files[f"{name}.f64"] = _write(role_dir / f"{name}.f64", m)
        del m
    src_sha = _sha(C.TICKERHISTORY)
    role_manifest = {
        "schema": ROLE_SCHEMA, "status": "complete", "instrument_namespace": "spiderrock.securityID",
        "close_basis": "f64(raw-f32-close)*f64-cumulReturnFactor",
        "close_basis_note": ("atx-db alpha_panel: vendor daily totalReturn chained backward from the line's last raw close; "
                             "factor-break-v1 repaired on mass sessions; sid0-bracket-v1 identity repair"),
        "volume_basis": "raw-share-volume", "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
        "common_stock_verified": False, "historical_vintage_verified": False,
        "dates": d, "instruments": n, "score_begin": score_begin, "score_end": d,
        "score_start_ns": int(sess_ns[score_begin]), "score_end_ns": int(sess_ns[-1] + NS_DAY),
        "source_sha256": src_sha, "membership_recipe": json.dumps(MEMBERSHIP, sort_keys=True, separators=(",", ":")),
        "sources": {"panel_stage": panel_stage(),
                    "panel_manifest_sha256": C.sha256_file(C.build_root() / panel_stage() / "manifest.json")},
        "declared_output_bytes": d * n * 26 + d * 8 + n * 8, "files": files,
        "producer": "atx_db.alpha_panel.export_impl", "window": {"first": str(lo), "score_start": str(sessions[score_begin]),
                                                                 "last": str(hi)},
    }
    role_text = json.dumps(role_manifest, indent=2, sort_keys=True)
    (role_dir / "manifest.json").write_text(role_text, encoding="utf-8", newline="\n")
    role_sha = hashlib.sha256(role_text.encode("utf-8")).hexdigest()

    field_rows, field_files = [], {}
    groups = {"grp_sic2", "grp_ff12", "grp_ff49"}
    for name in fields:
        spec = FIELD_SPECS[name]
        if name in groups:
            labels = [r[0] for r in con.execute(f"""SELECT DISTINCT {name} FROM read_parquet('{panel}', hive_partitioning = false)
                     WHERE {name} IS NOT NULL""").fetchall()]
            cmap = _code_map(labels)
            case = " ".join(f"WHEN {name} = {json.dumps(str(k)) if isinstance(k, str) else k} THEN {v}" for k, v in cmap.items())
            case = case.replace('"', "'")
            m = matrix(f"CASE {case} END" if cmap else "NULL", "<f8", np.nan, valid)
        else:
            where = valid
            if name in ISSUER_ITEMS and issuer_lines == "primary":
                where = f"({valid}) AND coalesce(is_issuer_primary, false)"
            m = matrix(f'CAST("{name}" AS DOUBLE)', "<f8", np.nan, where)
        m[present == 0] = np.nan
        info = _write(fields_dir / f"{name}.f64", m)
        mem = member.astype(bool)
        cov = float(np.isfinite(m[mem]).mean()) if mem.any() else None
        del m
        field_files[f"{name}.f64"] = info
        field_rows.append({
            "name": name, "file": f"{name}.f64", "sha256": info["sha256"], "dtype": "<f8", "layout": "date-major",
            "shape": [d, n], "point_in_time": True, "non_pit_aspects": [], "units": spec["units"], "clock": spec["clock"],
            "staleness": spec.get("staleness", spec["clock"]), "source_columns": [spec["src"]],
            "definition": spec.get("definition", spec["src"]) + (
                "; issuer items on the issuer's most liquid linked line only" if name in ISSUER_ITEMS and issuer_lines == "primary" else ""),
            "coverage": {"finite_member_frac": cov},
        })
        print(name, cov, flush=True)
    fm = {
        "schema": FIELDS_SCHEMA, "status": "complete",
        "role": {"manifest_sha256": role_sha, "sessions_sha256": files["sessions.i64"]["sha256"],
                 "ids_sha256": files["ids.u64"]["sha256"], "dates": d, "instruments": n},
        "fields": field_rows, "files": field_files, "producer": "atx_db.alpha_panel.export_impl",
        "sources": role_manifest["sources"],
    }
    (fields_dir / "manifest.json").write_text(json.dumps(fm, indent=2, sort_keys=True), encoding="utf-8", newline="\n")
    fields_sha = hashlib.sha256((fields_dir / "manifest.json").read_bytes()).hexdigest()
    con.close()
    return {"role_dir": str(role_dir), "role_manifest_sha256": role_sha, "fields_dir": str(fields_dir),
            "fields_manifest_sha256": fields_sha, "dates": d, "instruments": n, "score_begin": score_begin}


# --- aligned export: panel fields on the axes of an existing consumer role (request section 8 vi) --------------

ALIGN_SKIP = {"session_date", "security_id", "sidx", "ticker", "member", "adv_rank", "finra_issue_name", "cik",
              "fund_form", "fund_clock_basis", "fund_clock", "fund_period_end", "si_settlement", "shares_obs_date"}
# fields whose value or presence uses information that is not point in time (declared per manifest entry)
NON_PIT = {
    "directory_type": "Nasdaq Trader directory snapshot 2026-09-18 (current lines only)",
    "directory_etf": "Nasdaq Trader directory snapshot 2026-09-18 (current lines only)",
    "security_type": "falls back to the 2026-09-18 directory snapshot when no dated FINRA name classifies the line",
    "is_etf": "directory ETF flag of the 2026-09-18 snapshot is one input",
    "is_common": "security_type fallback to the 2026-09-18 directory snapshot",
    "delisting_date": "the line's last vendor session (known only after it)",
    "listing_date": "first vendor session (left-censored at 2012-03-26: NULL)",
}


def _role_axes(role_dir: Path) -> dict[str, Any]:
    blob = (role_dir / "manifest.json").read_bytes()
    m = json.loads(blob)
    if m.get("schema") != ROLE_SCHEMA or m.get("status") != "complete":
        raise SystemExit(f"{role_dir} is not a complete {ROLE_SCHEMA} role")
    sess = np.fromfile(role_dir / "sessions.i64", dtype="<i8")
    ids = np.fromfile(role_dir / "ids.u64", dtype="<u8").astype(np.int64)
    d, n = len(sess), len(ids)
    present = np.fromfile(role_dir / "present.u8", dtype="u1").reshape(d, n)
    member = np.fromfile(role_dir / "member.u8", dtype="u1").reshape(d, n)
    days = sess // NS_DAY
    if days[-1] >= (SEAL - EPOCH).days:
        raise SystemExit(f"role reaches the {SEAL} seal")
    return {"manifest_sha256": hashlib.sha256(blob).hexdigest(), "sessions_sha256": _sha(role_dir / "sessions.i64"),
            "ids_sha256": _sha(role_dir / "ids.u64"), "days": days, "ids": ids, "present": present,
            "member": member.astype(bool), "d": d, "n": n, "path": str(role_dir.resolve()),
            "first": str(EPOCH + dt.timedelta(days=int(days[0]))), "last": str(EPOCH + dt.timedelta(days=int(days[-1])))}


def align(role_dir: Path, out: Path, fields: list[str] | None, issuer_lines: str = "primary",
          link_tiers: str = "strict,name") -> dict[str, Any]:
    """Write panel columns as ``atx.research-role-fields/v1`` on an existing role's own axes (never re-projected)."""
    ax = _role_axes(role_dir)
    con = C.connect(memory="500MB", threads=2)
    root = C.build_root()
    panel = (root / panel_stage() / "*" / "*.parquet").as_posix()
    schema = con.execute(f"DESCRIBE SELECT * FROM read_parquet('{panel}', union_by_name = true)").fetchall()
    types = {r[0]: r[1] for r in schema}
    if fields is None:
        fields = [c for c in types if c not in ALIGN_SKIP]
    unknown = [f for f in fields if f not in types]
    if unknown:
        raise SystemExit(f"not panel columns: {unknown}")
    tiers = [t.strip() for t in link_tiers.split(",") if t.strip()]
    tier_sql = ", ".join(f"'{t}'" for t in tiers)
    events = (root / "fundamentals" / "events.parquet").as_posix()
    issuer_cols = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{events}')").fetchall()}
    issuer_cols |= {"sue", "sic", "grp_sic2", "grp_ff12", "grp_ff49", "me_company", "filer_regime", "fin_template",
                    "is_fpi", "is_adr_likely", "is_spac", "share_class_group_id", "is_issuer_primary"}
    issuer_cols -= {"cik"}
    ids_list = [int(x) for x in ax["ids"]]
    con.execute("CREATE TEMP TABLE ax AS SELECT unnest(?::BIGINT[]) AS security_id", [ids_list])
    d, n, days, idv = ax["d"], ax["n"], ax["days"], ax["ids"]
    lo = EPOCH + dt.timedelta(days=int(days[0]))
    hi = EPOCH + dt.timedelta(days=int(days[-1]))

    def matrix(expr: str, where: str) -> np.ndarray:
        m = np.full((d, n), np.nan, dtype="<f8")
        for y in range(lo.year, hi.year + 1):
            rows = con.execute(f"""
                SELECT date_diff('day', DATE '1970-01-01', p.session_date) AS dday, p.security_id, {expr} AS v
                FROM read_parquet('{(root / panel_stage() / f'year={y}' / '*.parquet').as_posix()}', union_by_name = true) p
                JOIN ax USING (security_id)
                WHERE p.session_date BETWEEN DATE '{lo}' AND DATE '{hi}' AND ({where})""").fetchnumpy()
            if not len(rows["v"]):
                continue
            dd = np.asarray(rows["dday"], dtype=np.int64)
            ss = np.asarray(rows["security_id"], dtype=np.int64)
            ri = np.minimum(np.searchsorted(days, dd), d - 1)
            ci = np.minimum(np.searchsorted(idv, ss), n - 1)
            ok = (days[ri] == dd) & (idv[ci] == ss)
            v = rows["v"]
            if hasattr(v, "filled"):
                v = v.astype("<f8").filled(np.nan)   # integer columns with NULLs come back as int masked arrays
            m[ri[ok], ci[ok]] = np.asarray(v, dtype="<f8")[ok]
        return m

    out.mkdir(parents=True, exist_ok=False)
    rows_out, files, codes = [], {}, {}
    present = ax["present"].astype(bool)
    for name in fields:
        t = types[name].upper()
        is_issuer = name in issuer_cols
        where = f"coalesce(link_tier IN ({tier_sql}), false)" if is_issuer else "TRUE"
        if is_issuer and issuer_lines == "primary" and name not in ("is_issuer_primary", "share_class_group_id"):
            where += " AND coalesce(is_issuer_primary, false)"
        if t.startswith("VARCHAR"):
            labels = sorted(r[0] for r in con.execute(f"""SELECT DISTINCT "{name}" FROM read_parquet('{panel}', union_by_name = true)
                                                           WHERE "{name}" IS NOT NULL""").fetchall())
            cmap = {lab: float(i + 1) for i, lab in enumerate(labels)}
            codes[name] = cmap
            case = " ".join(f"""WHEN "{name}" = '{str(k).replace("'", "''")}' THEN {v}""" for k, v in cmap.items())
            expr = f"CASE {case} END" if cmap else "NULL"
        elif t.startswith("BOOLEAN"):
            expr = f'CASE WHEN "{name}" THEN 1.0 WHEN NOT "{name}" THEN 0.0 END'
        elif t.startswith("DATE"):
            expr = f"""date_diff('day', DATE '1970-01-01', "{name}")"""
        elif t.startswith("TIMESTAMP"):
            expr = f'epoch("{name}")'
        else:
            expr = f'CAST("{name}" AS DOUBLE)'
        m = matrix(expr, where)
        m[~present] = np.nan
        info = _write(out / f"{name}.f64", m)
        cov = float(np.isfinite(m[ax["member"]]).mean()) if ax["member"].any() else None
        del m
        files[f"{name}.f64"] = info
        non_pit = []
        if name in NON_PIT:
            non_pit.append(NON_PIT[name])
        if is_issuer and "backfill" in tiers:
            non_pit.append("issuer link via the snapshot-run-backfill-v1 tier (2026 snapshot choice of CIK)")
        units = ("categorical code (see codes)" if name in codes else "days since 1970-01-01" if t.startswith("DATE")
                 else "0/1" if t.startswith("BOOLEAN") else "seconds since 1970-01-01 UTC" if t.startswith("TIMESTAMP")
                 else "panel units")
        rows_out.append({
            "name": name, "file": f"{name}.f64", "sha256": info["sha256"], "dtype": "<f8", "layout": "date-major",
            "shape": [d, n], "point_in_time": not non_pit, "non_pit_aspects": non_pit,
            "units": units,
            "clock": ("issuer: latest event with clock < 22:00 UTC of session d-1, link known by the session mark, "
                      f"link tiers {tiers}" + ("; issuer's most liquid linked line only" if issuer_lines == "primary" else ""))
                     if is_issuer else "panel as-of rule (docs/ALPHA_PANEL.md)",
            "source_columns": [f"alpha_panel.panel.{name}"], "coverage": {"finite_member_frac": cov}})
        print(name, cov, flush=True)
    fm = {"schema": FIELDS_SCHEMA, "status": "complete",
          "role": {**{k: ax[k] for k in ("path", "manifest_sha256", "sessions_sha256", "ids_sha256", "d", "n", "first", "last")},
                   # consumer binding keys (strategy_ic_admission.cpp bind_fields), same as export()
                   "dates": ax["d"], "instruments": ax["n"]},
          "fields": rows_out, "files": files, "codes": codes, "producer": "atx_db.alpha_panel.export_impl.align",
          "code": C.code_identity("export_impl", "panel", "common"), "link_tiers": tiers, "issuer_lines": issuer_lines,
          "sources": {"panel_manifest": C.output_hashes(root / panel_stage(), "manifest.json"),
                      "stage_manifests": {p.parent.name: C.sha256_file(p) for p in sorted(root.glob("*/manifest.json"))}},
          "seal": str(SEAL), "coverage_basis": "member.u8 cells of the role"}
    (out / "manifest.json").write_text(json.dumps(fm, indent=2, sort_keys=True), encoding="utf-8", newline="\n")
    return {"fields_dir": str(out), "fields": len(rows_out),
            "manifest_sha256": hashlib.sha256((out / "manifest.json").read_bytes()).hexdigest()}


def align_stage(role_dir: Path, out: Path, stage: str = "gold", fields: list[str] | None = None) -> dict[str, Any]:
    """Stream numeric gold/characteristic fields onto sealed consumer axes.

    Only dates on the role are scanned; the 2024+ warehouse features are never
    exported. Missing securities/sessions and absent role cells stay NaN. Memory
    maps bound resident matrix memory and atomic file renames publish each field.
    """
    if stage not in ("gold", "characteristics"):
        raise ValueError("feature stage must be gold or characteristics")
    ax = _role_axes(role_dir)
    if not ax["d"] or not ax["n"] or np.any(np.diff(ax["days"]) <= 0) or np.any(np.diff(ax["ids"]) <= 0):
        raise ValueError("consumer axes must be nonempty, sorted and unique")
    root = C.build_root()
    man_path = root / stage / "manifest.json"
    manifest = C.read_json(man_path)
    if manifest.get("status") != "complete":
        raise ValueError("feature stage is not complete")
    stage_sha = C.sha256_file(man_path)
    glob = (root / stage / "year=*" / "*.parquet").as_posix()
    con = C.connect(memory="220MB", threads=1)
    try:
        types = dict((r[0], r[1]) for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{glob}')").fetchall())
        if fields is None:
            fields = [c for c in types if c.startswith(("g_", "c_", "ctl_"))] if stage == "gold" else [
                c for c in types if c in R.by_name()]
        if not fields or len(fields) != len(set(fields)):
            raise ValueError("fields must be nonempty and unique")
        if any(c not in types or types[c] not in ("DOUBLE", "FLOAT") for c in fields):
            raise ValueError("only floating-point feature columns can be exported")
        out.mkdir(parents=True, exist_ok=False)
        d, n = ax["d"], ax["n"]
        present, member = ax["present"].astype(bool), ax["member"]
        con.execute("CREATE TEMP TABLE role_ids AS SELECT unnest(?::BIGINT[]) AS security_id", [ax["ids"].tolist()])
        lo = EPOCH + dt.timedelta(days=int(ax["days"][0]))
        hi = EPOCH + dt.timedelta(days=int(ax["days"][-1]))
        files, rows_out = {}, []
        for name in fields:
            path = out / f"{name}.f64"
            tmp = path.with_name(path.name + ".partial")
            matrix = np.memmap(tmp, mode="w+", dtype="<f8", shape=(d, n))
            for offset in range(0, d, 64):
                matrix[offset:offset + 64] = np.nan
            # Calendar-month chunks avoid fetching millions of narrow rows at once.
            cur = lo.replace(day=1)
            while cur <= hi:
                nxt = dt.date(cur.year + (cur.month == 12), cur.month % 12 + 1, 1)
                year_glob = root / stage / f"year={cur.year}" / "*.parquet"
                if list(year_glob.parent.glob("*.parquet")):
                    got = con.execute(f"""SELECT date_diff('day', DATE '1970-01-01', session_date) AS day,
                        security_id, "{name}" AS value FROM read_parquet('{year_glob.as_posix()}')
                        JOIN role_ids USING(security_id) WHERE session_date >= ? AND session_date < ?""",
                                      [max(cur, lo), min(nxt, hi + dt.timedelta(days=1))]).fetchnumpy()
                    dd, sid = np.asarray(got["day"]), np.asarray(got["security_id"])
                    ri, ci = np.searchsorted(ax["days"], dd), np.searchsorted(ax["ids"], sid)
                    valid = (ri < d) & (ci < n)
                    ri, ci, pos = ri[valid], ci[valid], np.flatnonzero(valid)
                    valid2 = (ax["days"][ri] == dd[pos]) & (ax["ids"][ci] == sid[pos]) & present[ri, ci]
                    values = got["value"]
                    if hasattr(values, "filled"):
                        values = values.filled(np.nan)
                    matrix[ri[valid2], ci[valid2]] = np.asarray(values)[pos[valid2]]
                cur = nxt
            finite, total = 0, 0
            for offset in range(0, d, 64):
                mask = member[offset:offset + 64]
                vals = matrix[offset:offset + 64]
                finite += int(np.isfinite(vals[mask]).sum())
                total += int(mask.sum())
            matrix.flush()
            del vals
            matrix._mmap.close()  # Windows cannot rename an open mapping, including its sliced views.
            del matrix
            os.replace(tmp, path)
            info = {"bytes": path.stat().st_size, "sha256": _sha(path)}
            files[path.name] = info
            rows_out.append({"name": name, "file": path.name, "sha256": info["sha256"], "dtype": "<f8",
                "layout": "date-major", "shape": [d, n], "point_in_time": True, "non_pit_aspects": [],
                "units": "standardized score" if stage == "gold" and not name.startswith("ctl_") else "raw feature units",
                "clock": "known by 22:00 UTC of decision session; trailing and same-session inputs only",
                "source_columns": [f"alpha_panel.{stage}.{name}"],
                "source_quality_flags": manifest.get("feature_quality_flags", {}).get(name,
                    feature_quality_flags(R.by_name()[name]) if stage == "characteristics" else []),
                "source_vintages_certified": manifest.get("source_vintages_certified", False),
                "coverage": {"finite_member_frac": finite / total if total else None}})
        if C.sha256_file(man_path) != stage_sha:
            raise ValueError("feature stage changed during export")
        payload = {"schema": FIELDS_SCHEMA, "status": "complete", "producer": "atx_db.alpha_panel.export_impl.align_stage",
            "role": {**{k: ax[k] for k in ("path", "manifest_sha256", "sessions_sha256", "ids_sha256", "d", "n", "first", "last")},
                     "dates": d, "instruments": n}, "fields": rows_out, "files": files, "codes": {},
            "seal": str(SEAL), "sources": {"feature_stage": stage, "feature_manifest_sha256": stage_sha},
            "quality_status": manifest.get("quality_status", "raw research candidates; validation required"),
            "predictive_power_certified": manifest.get("predictive_power_certified", False),
            "code": C.code_identity("export_impl", "gold", "common"), "coverage_basis": "member.u8 cells of the role"}
        C.write_json_atomic(out / "manifest.json", payload)
        return {"fields_dir": str(out), "fields": len(fields), "manifest_sha256": C.sha256_file(out / "manifest.json")}
    finally:
        con.close()


def main(argv: list[str] | None = None) -> int:
    actual = sys.argv[1:] if argv is None else argv
    if actual and actual[0] == "align-stage":
        ap = argparse.ArgumentParser()
        ap.add_argument("cmd")
        ap.add_argument("--role", type=Path, required=True)
        ap.add_argument("--out", type=Path, required=True)
        ap.add_argument("--stage", choices=("gold", "characteristics"), default="gold")
        ap.add_argument("--fields", default=None)
        a = ap.parse_args(actual)
        print(json.dumps(align_stage(a.role, a.out, a.stage, a.fields.split(",") if a.fields else None), indent=2))
        return 0
    if argv is None and len(sys.argv) > 1 and sys.argv[1] == "align":
        ap = argparse.ArgumentParser()
        ap.add_argument("cmd")
        ap.add_argument("--role", required=True)
        ap.add_argument("--out", required=True)
        ap.add_argument("--fields", default=None)
        ap.add_argument("--issuer-lines", choices=("primary", "all"), default="primary")
        ap.add_argument("--link-tiers", default="strict,name")
        a = ap.parse_args()
        res = align(Path(a.role), Path(a.out), a.fields.split(",") if a.fields else None, a.issuer_lines, a.link_tiers)
        print(json.dumps(res, indent=2))
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--score-start", default=str(C.COVERAGE_START))
    ap.add_argument("--end", default="2023-12-31")
    ap.add_argument("--fields", default=",".join(DEFAULT_FIELDS))
    ap.add_argument("--issuer-lines", choices=("primary", "all"), default="primary",
                    help="issuer items on the issuer's most liquid line only (scorecard semantics) or on every linked line")
    args = ap.parse_args(argv)
    res = export(Path(args.out), dt.date.fromisoformat(args.score_start), dt.date.fromisoformat(args.end),
                 [f for f in args.fields.split(",") if f], args.issuer_lines)
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
