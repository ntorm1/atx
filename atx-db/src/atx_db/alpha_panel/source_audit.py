"""Read-only coverage and bronze provenance inventory for the US equity alpha panel.

Coverage measures availability of inputs, never predictive performance. No
forward returns past the frozen research cutoff are read.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
from pathlib import Path
from typing import Any

from . import common as C

SOURCES = {
    "fundamentals_v10": ("events.parquet", "available_at"),
    "short_interest": ("si.parquet", "dissemination_date"),
    "short_volume": ("year=*/*.parquet", "trade_date"),
    "earnings_calendar": ("announcements.parquet", "available_at"),
    "sec_filings": ("filings.parquet", "available_at"),
    "thirteenf": ("agg_asof45.parquet", "available_at"),
    "insider": ("transactions/year=*/*.parquet", "available_at"),
    "ftd": ("year=*/*.parquet", "available_at"),
    "regsho_threshold": ("year=*/*.parquet", "available_at"),
    "options": ("year=*/*.parquet", "available_at"),
    "reference": ("fx_daily.parquet", "available_at"),
}


def measure_source(con, stage: str, pattern: str, clock: str) -> dict[str, Any]:
    root = C.build_root()
    files = sorted((root / stage).glob(pattern))
    if not files:
        return {"status": "missing"}
    glob = (root / stage / pattern).as_posix()
    columns = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{files[0].as_posix()}',hive_partitioning=false)").fetchall()}
    if clock not in columns:
        return {"status": "clock_column_missing", "expected_clock": clock, "files": len(files)}
    rows = con.execute(f"""SELECT year("{clock}"),count(*),min("{clock}"),max("{clock}")
                           FROM read_parquet('{glob}',hive_partitioning=false)
                           GROUP BY 1 ORDER BY 1""").fetchall()
    manifest = root / stage / "manifest.json"
    return {"status": "measured", "files": len(files), "bytes": sum(p.stat().st_size for p in files),
            "clock": clock, "manifest_sha256": C.sha256_file(manifest) if manifest.exists() else None,
            "annual": [{"year": y, "rows": n, "first_available": str(a), "last_available": str(b)}
                       for y, n, a, b in rows], "rows": sum(row[1] for row in rows)}


def audit() -> dict[str, Any]:
    root = C.build_root()
    out: dict[str, Any] = {"schema": "atx.alpha-panel.source-coverage/v1",
                           "measured_at_utc": dt.datetime.now(dt.UTC).isoformat(),
                           "requested_start": "2020-01-01", "requested_end": "2026-12-31",
                           "sources": {}, "panels": {}, "bronze": {},
                           "credential_environment_names": sorted(k for k in os.environ if any(
                               word in k.upper() for word in ("ORATS", "POLYGON", "MASSIVE", "API_KEY", "TOKEN")))}
    con = C.connect(memory="180MB", threads=1)
    try:
        calendar = [row[0] for row in con.execute(f"SELECT session_date FROM read_parquet('{C.calendar_path().as_posix()}') ORDER BY 1").fetchall()]
        out["actual_last_market_session"] = str(calendar[-1])
        out["future_market_sessions_missing"] = "No supplied vendor bars after the actual last session; 2026 is partial."
        for stage, (pattern, clock) in SOURCES.items():
            out["sources"][stage] = measure_source(con, stage, pattern, clock)
        for stage in ("prices", "panel", "panel_goldready"):
            annual = []
            for year in range(2020, 2027):
                files = sorted((root / stage / f"year={year}").glob("*.parquet"))
                if not files:
                    continue
                glob = (root / stage / f"year={year}" / "*.parquet").as_posix()
                extra = ("count(*) FILTER(WHERE ret_guarded),count(*) FILTER(WHERE sid0_repaired)" if stage == "prices" else
                         "count(*) FILTER(WHERE member_equity),count(*) FILTER(WHERE member_equity AND cik IS NOT NULL),"
                         "count(*) FILTER(WHERE member_equity AND link_tier='backfill'),"
                         "count(*) FILTER(WHERE member_equity AND \"at\" IS NOT NULL),"
                         "count(*) FILTER(WHERE member_equity AND si_shares IS NOT NULL),"
                         "count(*) FILTER(WHERE member_equity AND sv_total_volume IS NOT NULL),"
                         "count(*) FILTER(WHERE member_equity AND iv_atm_21d IS NOT NULL)")
                row = con.execute(f"SELECT count(*),min(session_date),max(session_date),count(distinct session_date),{extra} FROM read_parquet('{glob}',hive_partitioning=false)").fetchone()
                names = ("guarded_returns", "future_bracket_identity_repaired_rows") if stage == "prices" else (
                    "member_equity", "member_equity_linked", "member_equity_snapshot_linked", "member_equity_assets",
                    "member_equity_short_interest", "member_equity_short_volume", "member_equity_iv_21d")
                annual.append({"year": year, "rows": row[0], "first_session": str(row[1]), "last_session": str(row[2]),
                               "sessions": row[3], **dict(zip(names, row[4:], strict=True))})
            out["panels"][stage] = annual
        raw_manifest = C.PACKAGE_ROOT / "data" / "raw" / "finra_short_volume" / "manifest.csv"
        if raw_manifest.exists():
            with raw_manifest.open(newline="", encoding="utf-8") as fh:
                records = list(csv.DictReader(fh))
            successful = {dt.date.fromisoformat(r["date"]) for r in records if r["http_status"] == "200"}
            expected = {d for d in calendar if dt.date(2020, 1, 1) <= d <= calendar[-1]}
            out["bronze"]["finra_short_volume"] = {
                "manifest_sha256": C.sha256_file(raw_manifest), "successful_sessions": len(successful & expected),
                "missing_market_sessions_2020_onward": sorted(str(d) for d in expected - successful),
                "revision_vintage_verified": False}
        for path in (C.TICKERHISTORY, C.PACKAGE_ROOT / "data/cache/companyfacts.zip", C.PACKAGE_ROOT / "data/cache/submissions.zip"):
            if path.exists():
                out["bronze"][path.name] = C.file_identity(path)
        out["quality_limits"] = [
            "The legacy panel includes historical snapshot identity backfills; use panel_goldready for PIT issuer joins.",
            "Price/IV history and FINRA republications have no contemporaneous vintage proof.",
            "sid0 vendor repairs use future ticker brackets; source rows retain sid0_repaired.",
            "Lagged vendor shares remain a proxy; static vendor GICS is unsuitable for PIT industry classification.",
            "2024-2026 feature coverage is measured without evaluating sealed forward returns.",
        ]
        return out
    finally:
        con.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=C.build_root() / "validation/bronze_silver_2020_2026/source_coverage.json")
    args = ap.parse_args()
    report = audit()
    C.write_json_atomic(args.out, report)
    print(f"source coverage: {args.out}; latest market session {report['actual_last_market_session']}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
