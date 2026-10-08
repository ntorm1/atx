"""Refresh a PIT silver panel from cached bronze sources, resuming by verified month.

Run under the repository memory guard. The default output is isolated from the
published legacy panel; features must explicitly bind ``panel_goldready``.
No forward returns are acquired or evaluated by this stage.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any

from . import common as C
from . import panel as P

RULE = "pit-silver-cache-refresh-v1"


def source_signature(bind: P.Bindings) -> tuple[str, dict[str, Any]]:
    """Bind source manifests, actual key inputs and shared spine identities."""
    root = C.build_root()
    manifests = bind.input_manifests() + tuple(f"{s['stage']}/manifest.json" for s in P.ASOF_SOURCES)
    payload: dict[str, Any] = {
        "bindings": asdict(bind), "code": C.code_identity("panel", "common", "silver_refresh"),
        "manifests": {p: C.sha256_file(root / p) for p in sorted(set(manifests)) if (root / p).exists()},
        "inputs": {},
    }
    paths = [C.calendar_path(), root / bind.link_table,
             root / bind.fund_stage / "events.parquet", root / bind.fund_stage / "sic_events.parquet"]
    paths += sorted((root / "_tmp" / "panel_core").glob("*.parquet"))
    paths += sorted((root / "_tmp" / "panel_member").glob("*.parquet"))
    for path in paths:
        if path.exists():
            payload["inputs"][path.relative_to(root).as_posix()] = C.file_identity(path)
    encoded = json.dumps(payload, sort_keys=True).encode()
    return hashlib.sha256(encoded).hexdigest(), payload


def validate_month(con, path: Path, year: int, month: int) -> dict[str, Any]:
    """Require the price spine, unique keys, PIT tiers, and lagged fundamental clocks."""
    src = path.as_posix()
    prices = (C.build_root() / "prices" / f"year={year}" / "*.parquet").as_posix()
    cal = C.calendar_path().as_posix()
    row = con.execute(f"""
        WITH c AS (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev_session
                   FROM read_parquet('{cal}')),
        p AS (SELECT * FROM read_parquet('{src}', hive_partitioning=false))
        SELECT count(*), count(DISTINCT (p.session_date,p.security_id)),
               count(*) FILTER(WHERE year(p.session_date)<>{year} OR month(p.session_date)<>{month}),
               count(*) FILTER(WHERE p.link_tier IS NOT NULL AND p.link_tier NOT IN ('strict','dated','name')),
               count(*) FILTER(WHERE p.fund_clock >= c.prev_session + INTERVAL 22 HOUR),
               count(*) FILTER(WHERE p.member_equity),
               count(*) FILTER(WHERE p.member_equity AND p.cik IS NOT NULL),
               count(*) FILTER(WHERE p.member_equity AND p.at IS NOT NULL),
               min(p.session_date), max(p.session_date)
        FROM p JOIN c USING(session_date)
    """).fetchone()
    expected = con.execute(f"""SELECT count(*) FROM read_parquet('{prices}',hive_partitioning=false)
                              WHERE month(session_date)={month}""").fetchone()[0]
    if row[0] != expected or row[0] != row[1] or any(row[i] for i in (2, 3, 4)):
        raise ValueError(f"silver month validation failed: {year}-{month:02d}, metrics={row}, price_rows={expected}")
    mismatch = con.execute(f"""
        SELECT count(*) FROM (
          (SELECT session_date,security_id FROM read_parquet('{src}',hive_partitioning=false)
           EXCEPT SELECT session_date,security_id FROM read_parquet('{prices}',hive_partitioning=false)
                  WHERE month(session_date)={month})
          UNION ALL
          (SELECT session_date,security_id FROM read_parquet('{prices}',hive_partitioning=false)
                  WHERE month(session_date)={month}
           EXCEPT SELECT session_date,security_id FROM read_parquet('{src}',hive_partitioning=false)))
    """).fetchone()[0]
    if mismatch:
        raise ValueError(f"silver price spine mismatch: {year}-{month:02d}: {mismatch}")
    return {"rows": row[0], "member_equity": row[5], "member_equity_linked_pit": row[6],
            "member_equity_with_assets": row[7], "first_session": str(row[8]), "last_session": str(row[9]),
            "duplicate_keys": row[0] - row[1], "non_pit_links": row[3], "future_fundamental_clocks": row[4]}


def reusable_month(path: Path, receipt: Path, signature: str) -> bool:
    if not path.exists() or not receipt.exists():
        return False
    saved = C.read_json(receipt)
    return saved.get("source_signature") == signature and saved.get("sha256") == C.sha256_file(path)


def run(years: list[int], months: list[int], out_stage: str = "panel_goldready") -> dict[str, Any]:
    if out_stage == "panel":
        raise ValueError("silver refresh requires an isolated output stage")
    bind = P.bindings(out_stage=out_stage)
    signature, sources = source_signature(bind)
    root = C.build_root()
    out = C.stage_dir(out_stage)
    checks = out / "_receipts"
    checks.mkdir(exist_ok=True)
    report: dict[str, Any] = {"rule": RULE, "source_signature": signature, "source_bindings": sources,
                              "bindings": {**asdict(bind), "link_tiers": list(P.PIT_LINK_TIERS)},
                              "months": {}, "status": "building",
                              "quality_limits": ["Vendor prices and IV have no historical vintage verification.",
                                  "sid0 repairs use later ticker brackets; retain source quality flags.",
                                  "Vendor lagged shares are a proxy, not historical filing vintages.",
                                  "No labels or IC measurements after 2023-12-29."]}
    C.write_json_atomic(out / "refresh_progress.json", report)
    for year in years:
        for month in months:
            path = out / f"year={year}" / f"panel-{month:02d}.parquet"
            receipt = checks / f"{year}-{month:02d}.json"
            if reusable_month(path, receipt, signature):
                report["months"][f"{year}-{month:02d}"] = C.read_json(receipt)
                print(f"{year}-{month:02d}: reused", flush=True)
                continue
            name = f"{out_stage}_{year}_{month:02d}.duckdb"
            con = C.connect(memory="450MB", threads=1, db_file=name)
            started = time.perf_counter()
            try:
                sessions = con.execute(f"""SELECT count(*) FROM read_parquet('{C.calendar_path().as_posix()}')
                                          WHERE year(session_date)={year} AND month(session_date)={month}""").fetchone()[0]
                if not sessions:
                    continue
                assembled = P.assemble_month(con, year, month, bind)
                metrics = validate_month(con, path, year, month)
                saved = {**assembled, **metrics, "seconds": round(time.perf_counter() - started, 2),
                         "source_signature": signature, "sha256": C.sha256_file(path)}
                C.write_json_atomic(receipt, saved)
                report["months"][f"{year}-{month:02d}"] = saved
                C.write_json_atomic(out / "refresh_progress.json", report)
                print(f"{year}-{month:02d}: {metrics['rows']} rows, {saved['seconds']}s", flush=True)
            finally:
                con.close()
                for scratch in (root / "_tmp").glob(name + "*"):
                    scratch.unlink(missing_ok=True)
    report["status"] = "complete"
    report["requested_years"] = years
    report["requested_months"] = months
    C.write_stage_manifest(out_stage, P.PANEL_SCHEMA, ("panel", "common", "silver_refresh"), report)
    C.write_json_atomic(out / "refresh_progress.json", report)
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--years", default="2018-2026")
    ap.add_argument("--months", default="1-12")
    ap.add_argument("--out-stage", default="panel_goldready")
    args = ap.parse_args()
    def span(value: str) -> list[int]:
        a, _, b = value.partition("-")
        return list(range(int(a), int(b or a) + 1))
    run(span(args.years), span(args.months), args.out_stage)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
