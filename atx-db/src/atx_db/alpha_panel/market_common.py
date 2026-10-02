"""Shared helpers of lane MKT's stages (``market/``, ``indexes/``, ``classification/``, ``validation/``).

* :func:`project_prices` scans ``prices/`` once into ``_tmp/mkt/proj/bucket=NN.parquet`` (the columns the market
  stages read, plus the session index ``sidx``), cached by the SHA-256 of the prices manifest.
* :func:`publish_part` lets several modules publish into one directory: each writes its own
  ``<dir>/<part>_manifest.json`` (one lake registry stage per manifest) over its own files, with the SHA-256 of
  every input stage manifest under ``input_manifests_sha256``.
* :func:`split_events_sql` is the split / reverse-split subset of ``corporate_actions/vendor_events.parquet``.
"""

from __future__ import annotations

import datetime as dt
import json
import shutil
from pathlib import Path
from typing import Any

from . import common as C

BUCKETS = 16
SUB = 4  # a stage work unit is a quarter of a projection bucket (a guard stop loses at most one unit)
MARK = f"INTERVAL {C.MARK_HOUR_UTC} HOUR"
PROJ_COLUMNS = ("security_id", "session_date", "ticker", "open", "high", "low", "close", "volume", "dollar_volume",
                "ret", "ret_guarded", "return_factor", "fb_action", "shares_vendor", "prev_raw_close", "gap_days")


def tmp(name: str) -> Path:
    p = C.build_root() / "_tmp" / "mkt" / name
    p.mkdir(parents=True, exist_ok=True)
    return p


def manifest_sha(stage: str, name: str = "manifest.json") -> str | None:
    p = C.build_root() / stage / name
    return C.sha256_file(p) if p.exists() else None


def input_manifests(*stages: str) -> dict[str, str | None]:
    """``{stage: sha256 of <stage>/manifest.json}`` (``stage/file.json`` names a sub-manifest)."""
    out = {}
    for s in stages:
        stage, _, name = s.partition("/")
        out[s] = manifest_sha(stage, name or "manifest.json")
    return out


def calendar_sql() -> str:
    """Sessions with index and the previous session (``prev_session``) from ``calendar.parquet``."""
    return (f"(SELECT session_date, CAST(row_number() OVER (ORDER BY session_date) AS INTEGER) AS sidx, "
            f"lag(session_date) OVER (ORDER BY session_date) AS prev_session "
            f"FROM read_parquet('{C.calendar_path().as_posix()}'))")


def prices_glob() -> str:
    return (C.build_root() / "prices" / "year=*" / "*.parquet").as_posix()


def project_prices(con, receipt: dict[str, Any]) -> Path:
    out = tmp("proj")
    stamp = out / "_SUCCESS"
    ident = manifest_sha("prices") or "none"
    if stamp.exists() and stamp.read_text(encoding="utf-8") == ident:
        receipt["projection"] = "reused"
        return out
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    cols = ", ".join(f"p.{c}" for c in PROJ_COLUMNS)
    for b in range(BUCKETS):
        C.copy_to_parquet(con, f"""
            SELECT {cols}, cal.sidx, cal.prev_session
            FROM read_parquet('{prices_glob()}', hive_partitioning = false) p JOIN {calendar_sql()} cal USING (session_date)
            WHERE p.security_id % {BUCKETS} = {b}
            ORDER BY p.security_id, p.session_date""", out / f"bucket={b:02d}.parquet")
    stamp.write_text(ident, encoding="utf-8")
    receipt["projection"] = {"buckets": BUCKETS, "prices_manifest": ident}
    return out


def units() -> list[tuple[int, int]]:
    return [(b, k) for b in range(BUCKETS) for k in range(SUB)]


def sub_filter(k: int | None, col: str = "security_id") -> str:
    """SQL filter of sub-bucket ``k`` inside a projection bucket (TRUE for the whole bucket)."""
    return "TRUE" if k is None else f"({col} // {BUCKETS}) % {SUB} = {k}"


def bucket_path(b: int) -> str:
    return (tmp("proj") / f"bucket={b:02d}.parquet").as_posix()


def split_events_sql() -> str:
    """``security_id, ex_date, lsr`` (ln split ratio; 4:1 split -> ln 4) from the corporate-actions stage."""
    ev = (C.build_root() / "corporate_actions" / "vendor_events.parquet").as_posix()
    return (f"(SELECT security_id, ex_date, ln(split_ratio) AS lsr FROM read_parquet('{ev}') "
            f"WHERE kind IN ('split', 'reverse_split') AND split_ratio > 0)")


def publish_part(directory: str, part: str, schema: str, modules: tuple[str, ...], payload: dict[str, Any],
                 inputs: dict[str, str | None], pattern: str) -> Path:
    """Publish-last ``<directory>/<part>_manifest.json`` (one registry stage per manifest): schema, status, code
    identity, the SHA-256 of the files matching ``pattern`` (relative to ``directory``), bound input manifests."""
    d = C.stage_dir(directory)
    missing = [k for k, v in inputs.items() if v is None]
    if missing:
        raise RuntimeError(f"{part}: input manifests missing: {missing}")
    manifest = {"schema": schema, "status": "complete", "stage": part, "code": C.code_identity(*modules),
                "files": C.output_hashes(d, pattern), "input_manifests_sha256": inputs,
                "published_at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"), **payload}
    path = d / f"{part}_manifest.json"
    C.write_json_atomic(path, manifest)
    return path


def member_coverage(con, glob: str, exprs: dict[str, str], years: range = range(2019, 2027),
                    extra: dict[str, str] | None = None) -> dict[str, Any]:
    """Per year: member_equity cells of the panel and the share of them where each ``exprs`` value is true
    (``{name: SQL boolean over alias t}``), plus ``extra`` aggregates; ``glob`` may hold ``{y}`` for the year."""
    panel = (C.build_root() / "panel").as_posix()
    out: dict[str, Any] = {}
    for y in years:
        sel = ", ".join(f"avg(CASE WHEN {e} THEN 1.0 ELSE 0.0 END) AS \"{k}\"" for k, e in exprs.items())
        if extra:
            sel += ", " + ", ".join(f"{e} AS \"{k}\"" for k, e in extra.items())
        row = con.execute(f"""
            SELECT count(*) AS cells, {sel}
            FROM read_parquet('{panel}/year={y}/*.parquet', union_by_name = true, hive_partitioning = false) p
            LEFT JOIN read_parquet('{glob.format(y=y)}', hive_partitioning = false) t USING (session_date, security_id)
            WHERE p.member_equity AND year(p.session_date) = {y}
        """)
        cols = [c[0] for c in row.description]
        vals = row.fetchone()
        out[str(y)] = {c: (round(v, 6) if isinstance(v, float) else v) for c, v in zip(cols, vals)}
    return out


def resumable_dir(name: str, inputs: dict[str, Any], modules: tuple[str, ...]) -> Path:
    """``_tmp/mkt/<name>`` kept across runs while the input manifests and code are unchanged (a guard stop resumes
    at the first missing bucket); otherwise emptied."""
    d = tmp(name)
    code = {k: v["sha256_lf"] for k, v in C.code_identity(*modules).items() if isinstance(v, dict)}
    ident = {"inputs": inputs, "code": code}
    stamp = d / "_inputs.json"
    if stamp.exists() and C.read_json(stamp) == json.loads(json.dumps(ident, sort_keys=True, default=str)):
        return d
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    C.write_json_atomic(stamp, ident)
    return d
