"""Stage ``short_volume_ext``: FINRA CNMS daily short volume with exempt volume and per-facility flags.

Source: the CNMS files already landed by stage V (``data/raw/finra_short_volume/CNMSshvolYYYYMMDD.txt.gz``);
every file is decompressed and checked against the sha256 of its raw bytes in that landing's
``manifest.csv`` before use. Nothing is downloaded.

Mapping: the stage-V rule ``sv-ticker-on-trade-date-v2`` itself (``short_volume.map_year``: vendor ticker on
the trade date, exact then canonical, ambiguity and collisions unmapped, shared sid-0 repair).

Output ``short_volume_ext/year=YYYY/short_volume_ext.parquet`` (one row per ``(trade_date, symbol)``):
``security_id, symbol, trade_date, short_volume, short_exempt_volume, total_volume, market`` (the file's
``Market`` field as written), per-facility flags ``fac_b`` (FINRA/Nasdaq TRF Chicago), ``fac_q`` (FINRA/Nasdaq
TRF Carteret), ``fac_n`` (FINRA/NYSE TRF), ``fac_d`` (ADF), ``fac_other`` (any other code, verbatim),
``n_facilities``, ``map_basis``, ``sid_repaired``, ``available_at``, ``vintage_risk``.

Clock ``sv-ext-next-day-v1``: FINRA posts the file for trade date T in the evening of T (about 18:00 ET);
``available_at = T + 1 day 00:00 UTC`` (19:00-20:00 ET). Under the consumer rule (use at decision session d
only if ``available_at < 22:00 UTC of d-1``) this is the stage-V contract "trade date T is visible from
session T+1". ``vintage_risk`` is true on every row: the files were landed in 2026-09 and FINRA keeps no
revision history, so the first-published bytes cannot be proven.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import finra_fetch as F
from . import short_volume as SV
from . import shortflow_common as S

STAGE = "short_volume_ext"
SCHEMA = "atx.alpha-panel.short-volume-ext/v1"
CLOCK_RULE = "sv-ext-next-day-v1"
STALENESS_DAYS = 5
FACILITIES = {"B": "fac_b", "Q": "fac_q", "N": "fac_n", "D": "fac_d"}
MODULES = ("short_volume_ext", "short_volume", "shortflow_common", "common", "finra_fetch")


def parse_market(market: str | None) -> dict[str, Any]:
    """``'B,Q,N'`` -> per-facility flags, the count of distinct codes and any unknown codes (sorted, comma-joined)."""
    codes = [c.strip() for c in (market or "").split(",") if c.strip()]
    out: dict[str, Any] = {v: (k in codes) for k, v in FACILITIES.items()}
    other = sorted({c for c in codes if c not in FACILITIES})
    out["fac_other"] = ",".join(other) if other else None
    out["n_facilities"] = len(set(codes))
    return out


def _stage_raw(year: int, man: dict[str, dict[str, str]]) -> tuple[Path | None, dict[str, Any]]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    dest = S.tmp_dir("sv_ext") / f"raw_{year}.parquet"
    metas = sorted((k, v) for k, v in man.items() if k.startswith(str(year)) and int(v.get("http_status") or 0) == 200)
    if not metas:
        return None, {}
    ident = [(k, v["sha256_of_raw_bytes"]) for k, v in metas]
    stamp = dest.with_suffix(".json")
    if dest.exists() and stamp.exists() and C.read_json(stamp).get("files") == ident:
        return dest, C.read_json(stamp)["stats"] | {"reused": True}
    schema = pa.schema([("trade_date", pa.date32()), ("symbol", pa.string()), ("short_volume", pa.float64()),
                        ("short_exempt_volume", pa.float64()), ("total_volume", pa.float64()), ("market", pa.string())])
    agg = {"files": 0, "files_sha256_verified": 0, "rows": 0, "bad": 0, "date_mismatch": 0,
           "row_count_vs_manifest_mismatch": 0}
    tmp = dest.with_name(dest.name + ".partial")
    with pq.ParquetWriter(tmp, schema, compression="zstd") as w:
        for k, v in metas:
            body = F.read_gzip_verified(F.SV_RAW_DIR / v["file"], v["sha256_of_raw_bytes"])  # raises on mismatch
            agg["files_sha256_verified"] += 1
            rows, st = SV.parse_file(body, dt.date.fromisoformat(k))
            agg["files"] += 1
            agg["rows"] += len(rows)
            agg["bad"] += st["bad"]
            agg["date_mismatch"] += st["date_mismatch"]
            if len(rows) != int(v["rows"]):
                agg["row_count_vs_manifest_mismatch"] += 1
            if rows:
                cols = list(zip(*rows))
                w.write_table(pa.table([pa.array(c, t.type) for c, t in zip(cols, schema)], schema=schema))
    tmp.replace(dest)
    C.write_json_atomic(stamp, {"files": ident, "stats": agg})
    return dest, agg


def build(start: dt.date = SV.FIRST_CNMS, end: dt.date = SV.DEFAULT_END) -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"map_rule": SV.RULE, "clock_rule": CLOCK_RULE, "tickerhistory": C.file_identity(C.TICKERHISTORY)}
    man_path = F.SV_RAW_DIR / "manifest.csv"
    man = F.load_manifest(man_path)
    receipt["raw_manifest"] = {"path": str(man_path), "sha256": C.sha256_file(man_path), "dates": len(man)}
    con = S.connect("sv_ext")
    sessions = F.sessions(con)
    out = C.stage_dir(STAGE)
    stage_v = C.build_root() / "short_volume"
    years: dict[str, Any] = {}
    for y in C.years_between(start, end):
        raw, pst = _stage_raw(y, man)
        if raw is None:
            continue
        ys, ye = max(start, dt.date(y, 1, 1)), min(end, dt.date(y, 12, 31))
        th = S.tmp_dir("sv_ext") / f"th_{y}.parquet"
        th_dates = sorted({d for d in sessions if ys <= d <= ye} | {dt.date.fromisoformat(k) for k in man if k.startswith(str(y))})
        F.ticker_extract(con, th_dates, th)
        mapped = S.tmp_dir("sv_ext") / f"mapped_{y}.parquet"
        res = SV.map_year(con, raw, th, mapped)
        dest = out / f"year={y}" / "short_volume_ext.parquet"
        m = mapped.as_posix()
        n = C.copy_to_parquet(con, f"""
            SELECT security_id, symbol, trade_date, short_volume, short_exempt_volume, total_volume, market,
                   list_contains(string_split(market, ','), 'B') AS fac_b,
                   list_contains(string_split(market, ','), 'Q') AS fac_q,
                   list_contains(string_split(market, ','), 'N') AS fac_n,
                   list_contains(string_split(market, ','), 'D') AS fac_d,
                   nullif(array_to_string(list_sort(list_distinct(list_filter(string_split(market, ','),
                          x -> trim(x) <> '' AND trim(x) NOT IN ('B', 'Q', 'N', 'D')))), ','), '') AS fac_other,
                   len(list_distinct(list_filter(string_split(market, ','), x -> trim(x) <> ''))) AS n_facilities,
                   map_basis, sid_repaired,
                   CAST(trade_date + INTERVAL 1 DAY AS TIMESTAMP) AT TIME ZONE 'UTC' AS available_at,
                   true AS vintage_risk
            FROM read_parquet('{m}') ORDER BY trade_date, symbol""", dest)
        d = dest.as_posix()
        st = con.execute(f"""SELECT sum(short_exempt_volume) / nullif(sum(short_volume), 0),
                                    count(*) FILTER (WHERE short_exempt_volume > 0) / count(*),
                                    avg(fac_b::INT), avg(fac_q::INT), avg(fac_n::INT), avg(fac_d::INT),
                                    count(*) FILTER (WHERE fac_other IS NOT NULL),
                                    count(*) FILTER (WHERE short_exempt_volume > short_volume)
                             FROM read_parquet('{d}')""").fetchone()
        val = {}
        vpath = stage_v / f"year={y}" / "short_volume.parquet"
        if vpath.exists():
            v = con.execute(f"""
                SELECT count(*), count(*) FILTER (WHERE a.symbol IS NULL OR b.symbol IS NULL),
                       count(*) FILTER (WHERE a.security_id IS DISTINCT FROM b.security_id),
                       count(*) FILTER (WHERE a.short_exempt_volume IS DISTINCT FROM b.short_exempt_volume
                                          OR a.market IS DISTINCT FROM b.market)
                FROM read_parquet('{d}') a FULL JOIN read_parquet('{vpath.as_posix()}') b
                  ON a.trade_date = b.trade_date AND a.symbol = b.symbol""").fetchone()
            val = {"joined_rows": v[0], "rows_missing_either_side": v[1], "security_id_differs": v[2],
                   "exempt_or_market_differs": v[3]}
        years[str(y)] = {"rows": n, "parse": pst, "map": {k: res[k] for k in ("map_basis", "mapped_share_rows",
                                                                                 "mapped_share_total_volume", "ids", "trade_dates",
                                                                                 "first", "last")},
                         "exempt_share_of_short_volume": round(float(st[0]), 5) if st[0] is not None else None,
                         "rows_with_exempt_gt0": round(float(st[1]), 4), "share_fac_b": round(float(st[2]), 4),
                         "share_fac_q": round(float(st[3]), 4), "share_fac_n": round(float(st[4]), 4),
                         "share_fac_d": round(float(st[5]), 4), "rows_fac_other": int(st[6]),
                         "rows_exempt_gt_short": int(st[7]), "vs_stage_v": val}
        print(f"short_volume_ext {y}: {years[str(y)]['rows']} rows, vs V {val}", flush=True)
    glob = (out / "year=*" / "short_volume_ext.parquet").as_posix()
    receipt["years"] = years
    receipt["coverage_by_year"] = coverage(con, glob)
    receipt["rows_total"] = sum(v["rows"] for v in years.values())
    receipt["timings_s"] = {"total": round(time.perf_counter() - t0, 1)}
    payload = {
        "map_rule": SV.RULE, "clock_rule": CLOCK_RULE,
        "rule_text": "stage-V mapping sv-ticker-on-trade-date-v2 (short_volume.map_year) over sha-verified raw CNMS files",
        "clock_text": ("available_at = trade_date + 1 day 00:00 UTC; consumer uses at decision session d only if "
                       "available_at < 22:00 UTC of d-1 (= stage-V 'trade date T visible from session T+1')"),
        "facility_codes": {"B": "FINRA/Nasdaq TRF Chicago", "Q": "FINRA/Nasdaq TRF Carteret", "N": "FINRA/NYSE TRF",
                           "D": "FINRA ADF"},
        "staleness_rule": f"a trade date older than {STALENESS_DAYS} sessions is stale (daily file)",
        "staleness_days": STALENESS_DAYS,
        "vintage_risk": "true on every row: files landed 2026-09-27, FINRA keeps no revision history",
        "sources": {"raw_manifest": receipt["raw_manifest"], "files_sha256": {k: v["sha256_of_raw_bytes"] for k, v in sorted(man.items())
                                                                                  if int(v.get("http_status") or 0) == 200}},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def coverage(con, glob: str) -> dict[str, Any]:
    """Per year: share of member_equity cells (session d) with a mapped CNMS row for trade date d."""
    panel = C.build_root() / "panel"
    rows = con.execute(f"""
        WITH m AS (
            SELECT session_date, security_id FROM read_parquet('{(panel / 'year=*' / '*.parquet').as_posix()}', hive_partitioning = false)
            WHERE member_equity AND session_date >= DATE '2018-08-01'
        ),
        v AS (SELECT DISTINCT trade_date, security_id FROM read_parquet('{glob}', hive_partitioning = false) WHERE security_id IS NOT NULL)
        SELECT year(m.session_date), count(*), count(v.security_id)
        FROM m LEFT JOIN v ON v.trade_date = m.session_date AND v.security_id = m.security_id
        GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(y): {"member_equity_cells": n, "share_with_row": round(k / n, 4)} for y, n, k in rows}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", type=dt.date.fromisoformat, default=SV.FIRST_CNMS)
    ap.add_argument("--end", type=dt.date.fromisoformat, default=SV.DEFAULT_END)
    args = ap.parse_args(argv)
    rec = build(args.start, args.end)
    print(json.dumps({k: v for k, v in rec.items() if k in ("rows_total", "coverage_by_year", "timings_s")}, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
