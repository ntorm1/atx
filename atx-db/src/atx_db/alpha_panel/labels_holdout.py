"""Strict 2024 onward labels, accessible only after an authorized immutable gold freeze.

This module leaves the development labels' 2023 cutoff unchanged. Returns are
read only after gold_freeze.verify succeeds. Existing files are rebuilt unless
their own manifest pins the same freeze and source manifests.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import time
from pathlib import Path
from typing import Any

import duckdb

from . import common as C
from . import labels as L

HORIZONS = (5, 21, 63)
OOS_START = dt.date(2024, 1, 1)
SCHEMA = "atx.alpha-panel.labels-holdout/v1"
QUALITY_CODES = {"1": "complete observed horizon", "2": "complete observed terminal payoff",
                 "3": "unknown terminal payoff excluded", "4": "imputed terminal payoff excluded",
                 "5": "incomplete/invalid returns or missing trade anchor excluded",
                 "6": "latest available session censored", "7": "repaired decision identity excluded",
                 "8": "repaired trade anchor or forward identity excluded"}


def verify_freeze(path: Path, authorized_oos: bool) -> dict[str, Any]:
    if not authorized_oos:
        raise ValueError("holdout labels require explicit authorized frozen-holdout mode")
    from .gold_freeze import verify
    return verify(path, authorized_oos=authorized_oos)


def scan_holdout(con: duckdb.DuckDBPyConnection, files: list[Path], lo: dt.date, hi: dt.date,
                 cutoff: dt.date) -> int:
    """Scan within a previously verified freeze ceiling; require a real entry price."""
    if lo < OOS_START or hi > cutoff:
        raise ValueError(f"holdout scan {lo}..{hi} exceeds authorized window {OOS_START}..{cutoff}")
    if not files:
        raise FileNotFoundError("holdout panel files are missing")
    con.from_parquet([str(p) for p in files], hive_partitioning=False).create_view("holdout_input", replace=True)
    cols = {r[0] for r in con.execute("DESCRIBE holdout_input").fetchall()}
    if "raw_close" not in cols:
        raise ValueError("holdout labels require raw_close to verify the d+1 trade anchor")
    con.execute("""CREATE OR REPLACE TEMP TABLE w AS
        SELECT p.session_date,c.idx,p.security_id,
               CASE WHEN coalesce(q.repaired,false) OR coalesce(p.ret_guarded,false)
                          OR p.ret IS NULL OR NOT isfinite(p.ret) OR p.ret<=-1
                          OR (e.lidx IS NOT NULL AND c.idx>e.lidx) THEN NULL
                    ELSE ln(1+p.ret) END AS lr,
               e.lidx,e.dlret,e.dlret_observed,coalesce(q.repaired,false) AS source_repaired,
               coalesce(isfinite(p.raw_close) AND p.raw_close>0,false) AS anchor_valid
        FROM holdout_input p JOIN cal c USING(session_date)
        LEFT JOIN ev e USING(security_id)
        LEFT JOIN label_repairs q USING(session_date,security_id)
        WHERE p.session_date BETWEEN ? AND ?""", [lo, hi])
    n, unique, top = con.execute("SELECT count(*),count(DISTINCT(session_date,security_id)),max(session_date) FROM w").fetchone()
    if n != unique or (top is not None and top > cutoff):
        raise ValueError("holdout input has duplicate keys or exceeds the frozen ceiling")
    return n


def strict_sql(cut_idx: int, lo_idx: int, hi_idx: int) -> str:
    """Reuse strict label arithmetic and additionally require the d+1 traded close."""
    columns = ["l.session_date", "l.security_id"]
    for h in HORIZONS:
        columns += [f"CASE WHEN a.n_anchor=1 THEN l.fwd_{h} END AS fwd_{h}",
                    f"CAST(l.n_{h} AS SMALLINT) AS n_{h}",
                    f"CAST(CASE WHEN l.quality_{h} IN(6,7,8) THEN l.quality_{h} "
                    f"WHEN a.n_anchor<>1 THEN 5 ELSE l.quality_{h} END AS UTINYINT) AS quality_{h}"]
    return f"""WITH l AS ({L._label_sql(cut_idx,lo_idx,hi_idx,quality='strict')}),
        a AS (SELECT session_date,security_id,
              count(*) FILTER(WHERE anchor_valid) OVER(PARTITION BY security_id ORDER BY idx
                    RANGE BETWEEN 1 FOLLOWING AND 1 FOLLOWING) AS n_anchor FROM w)
        SELECT {','.join(columns)} FROM l JOIN a USING(session_date,security_id)"""


def _input_pins(root: Path, panel_stage: str) -> dict[str, str]:
    required = [f"{panel_stage}/manifest.json", "prices/manifest.json", "calendar.parquet"]
    pins = {}
    for rel in required:
        path = root / rel
        if not path.exists():
            raise FileNotFoundError(f"holdout source pin missing: {path}")
        pins[rel] = C.sha256_file(path)
    for rel in ("delisting/manifest.json", "delisting/events.parquet"):
        if (root / rel).exists():
            pins[rel] = C.sha256_file(root / rel)
    return pins


def build(freeze_path: Path, *, authorized_oos: bool = False, panel_stage: str = "panel_goldready",
          out_stage: str = "labels_holdout", memory: str = "300MB", threads: int = 1,
          force: bool = False) -> dict[str, Any]:
    frozen = verify_freeze(freeze_path, authorized_oos)  # MUST precede calendar or return reads.
    freeze_sha = C.sha256_file(freeze_path)
    if not re.fullmatch(r"labels_holdout\w*", out_stage):
        raise ValueError("holdout labels require an isolated labels_holdout output stage")
    if frozen.get("panel_stage") != panel_stage:
        raise ValueError("holdout panel stage does not match the immutable freeze")
    if frozen.get("horizons") != list(HORIZONS) or frozen.get("oos_start") != str(OOS_START):
        raise ValueError("holdout window/horizons do not match the frozen protocol")
    cutoff = dt.date.fromisoformat(frozen["oos_cutoff"])
    root = C.build_root()
    pins = _input_pins(root, panel_stage)
    panel_manifest = C.read_json(root / panel_stage / "manifest.json")
    if panel_manifest.get("status") != "complete":
        raise ValueError("holdout source panel is incomplete")
    out = C.stage_dir(out_stage)
    prior_path = out / "manifest.json"
    prior = C.read_json(prior_path) if prior_path.exists() else {}
    if prior and not force and (prior.get("freeze_sha256") != freeze_sha or prior.get("input_manifests_sha256") != pins):
        raise ValueError("holdout freeze/source changed: use an isolated stage or --force")
    # A restarted/failed build must never retain a prior complete publication.
    C.write_json_atomic(prior_path, {"schema": SCHEMA, "status": "building", "freeze_sha256": freeze_sha,
                                  "input_manifests_sha256": pins})
    scratch = f"{out_stage}.duckdb"
    con = C.connect(memory=memory, threads=threads, db_file=scratch)
    receipt: dict[str, Any] = {"cutoff": str(cutoff), "start": str(OOS_START), "panel_stage": panel_stage,
                              "label_policy": "strict", "rows_per_year": {}, "quality_attrition": {},
                              "timings_s": {}, "max_label_end": None}
    try:
        cal = C.load_calendar(con)
        if cutoff not in cal or cutoff > cal[-1] or cutoff < OOS_START:
            raise ValueError("frozen cutoff must be an available market session in the holdout window")
        idx_of = {d: i for i, d in enumerate(cal)}
        cut_idx = idx_of[cutoff]
        con.execute("CREATE TEMP TABLE cal(session_date DATE,idx BIGINT)")
        con.executemany("INSERT INTO cal VALUES(?,?)", [(d, i) for i, d in enumerate(cal) if i <= cut_idx])
        ev = root / "delisting/events.parquet"
        if ev.exists():
            con.from_parquet(str(ev), hive_partitioning=False).create_view("terminal_events")
            cols = {r[0] for r in con.execute("DESCRIBE terminal_events").fetchall()}
            observed = "NOT coalesce(e.dlret_imputed,true)" if "dlret_imputed" in cols else "false"
            con.execute(f"""CREATE TEMP TABLE ev AS SELECT e.security_id,c.idx lidx,e.dlret,
                              {observed} AS dlret_observed FROM terminal_events e JOIN cal c
                              ON e.last_session=c.session_date WHERE NOT coalesce(e.continued,false)""")
        else:
            con.execute("CREATE TEMP TABLE ev(security_id BIGINT,lidx BIGINT,dlret DOUBLE,dlret_observed BOOLEAN)")
        max_end = None
        for year in range(OOS_START.year, cutoff.year + 1):
            decisions = [(i, d) for i, d in enumerate(cal) if d.year == year and OOS_START <= d <= cutoff]
            if not decisions:
                continue
            lo_idx, hi_idx = decisions[0][0], decisions[-1][0]
            scan_hi = min(hi_idx + 1 + max(HORIZONS), cut_idx)
            files = [p for y in range(year, cal[scan_hi].year + 1)
                     for p in sorted((root / panel_stage / f"year={y}").glob("*.parquet"))]
            price_files = [p for y in range(year, cal[scan_hi].year + 1)
                           for p in sorted((root / "prices" / f"year={y}").glob("*.parquet"))]
            if not price_files:
                raise FileNotFoundError("strict holdout labels require underlying price identity repair flags")
            con.from_parquet([str(p) for p in price_files], hive_partitioning=False).create_view("holdout_prices", replace=True)
            if "sid0_repaired" not in {r[0] for r in con.execute("DESCRIBE holdout_prices").fetchall()}:
                raise ValueError("strict holdout labels require prices.sid0_repaired")
            con.execute("""CREATE OR REPLACE TEMP TABLE label_repairs AS SELECT session_date,security_id,true AS repaired
                           FROM holdout_prices WHERE sid0_repaired AND session_date BETWEEN ? AND ?""",
                        [cal[lo_idx], cal[scan_hi]])
            started = time.perf_counter()
            scan_holdout(con, files, cal[lo_idx], cal[scan_hi], cutoff)
            con.execute(f"CREATE OR REPLACE TEMP TABLE y AS {strict_sql(cut_idx,lo_idx,hi_idx)}")
            attrition = {}
            for h in HORIZONS:
                attrition[f"h{h}"] = {str(code): n for code, n in con.execute(f"SELECT quality_{h},count(*) FROM y GROUP BY 1").fetchall()}
                last = con.execute(f"SELECT max(session_date) FROM y WHERE fwd_{h} IS NOT NULL").fetchone()[0]
                if last is not None:
                    end_idx = idx_of[last] + 1 + h
                    if end_idx > cut_idx:
                        raise ValueError("holdout endpoint exceeds the frozen data ceiling")
                    max_end = max(max_end or 0, end_idx)
            dest = out / f"year={year}/labels.parquet"
            n = C.copy_to_parquet(con, "SELECT * FROM y ORDER BY session_date,security_id", dest, row_group_size=65536)
            receipt["rows_per_year"][str(year)] = n
            receipt["quality_attrition"][str(year)] = attrition
            receipt["timings_s"][str(year)] = round(time.perf_counter() - started, 2)
            receipt["max_label_end"] = str(cal[max_end]) if max_end is not None else None
            C.write_json_atomic(out / "progress.json", receipt)
            print(f"holdout labels {year}: {n} rows in {receipt['timings_s'][str(year)]}s", flush=True)
        verify_freeze(freeze_path, authorized_oos)
        if C.sha256_file(freeze_path) != freeze_sha or _input_pins(root, panel_stage) != pins:
            raise ValueError("holdout freeze or source changed during generation")
        # Remove out-of-window partitions when an explicitly forced narrower freeze is used.
        for path in out.glob("year=*/labels.parquet"):
            if int(path.parent.name.split("=")[1]) not in range(OOS_START.year, cutoff.year + 1):
                path.unlink()
        C.write_stage_manifest(out_stage, SCHEMA, ("labels_holdout", "labels", "common"), {
            "freeze_path": str(freeze_path.resolve()), "freeze_sha256": freeze_sha,
            "authorized_frozen_holdout": True, "label_policy": "strict", "label_cutoff": str(cutoff),
            "horizons": list(HORIZONS),
            "input_manifests_sha256": pins, "receipt": receipt, "quality_attrition": receipt["quality_attrition"],
            "quality_codes": QUALITY_CODES, "max_label_end": receipt["max_label_end"],
            "latest_market_session": str(cal[-1]), "frozen_cutoff": str(cutoff),
            "limitations": "Complete observed horizons only; terminal imputations excluded. Attrition introduces selection bias."})
        return receipt
    finally:
        con.close()
        for path in (root / "_tmp" / scratch, root / "_tmp" / (scratch + ".wal")):
            path.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--freeze", type=Path, required=True)
    ap.add_argument("--authorized-frozen-holdout", action="store_true")
    ap.add_argument("--panel-stage", default="panel_goldready")
    ap.add_argument("--out-stage", default="labels_holdout")
    ap.add_argument("--memory", default="300MB")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    build(args.freeze, authorized_oos=args.authorized_frozen_holdout, panel_stage=args.panel_stage,
          out_stage=args.out_stage, memory=args.memory, force=args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
