"""Stage V: FINRA Reg SHO daily short sale volume (CNMS consolidated) -> ``short_volume/``.

Source: ``https://cdn.finra.org/equity/regsho/daily/CNMSshvolYYYYMMDD.txt`` (consolidated NMS: all
FINRA TRFs + ADF, exchange-listed securities; first file 2018-08-01). Downloaded by ``download``
into ``atx-db/data/raw/finra_short_volume/`` (gzip at rest, append-only ``manifest.csv``).

Output ``short_volume/year=YYYY/short_volume.parquet`` (docs/ALPHA_PANEL.md, stage V):
``security_id BIGINT, symbol, trade_date DATE, short_volume, short_exempt_volume, total_volume``
(DOUBLE), one row per ``(trade_date, symbol)`` in the file; ``security_id`` is NULL when unmapped.

Symbol rule ``sv-ticker-on-trade-date-v2``: the ORATS ``securityID`` whose ``ticker_tk`` equals the
FINRA symbol on the trade date itself (case-sensitive exact match first; else the canonical form with
``.``, ``/``, ``-``, space removed, still case-sensitive, must name exactly one id that day; CNMS
writes BRK/B where ORATS writes BRK.B, and both mark preferreds with a lower-case ``p``). A form
carried by two or more ids that day is ambiguous and left unmapped. Two symbols mapping to one id
on one day: the single exact match keeps the id, the other rows are unmapped
(``map_basis = 'collision'``). Vendor rows filed under ``securityID = 0`` are re-attributed with the
shared ``sid0-bracketed-ticker-v2`` repair (``finra_fetch.sid0_repairs``, identical to stage P);
``sid_repaired`` flags rows mapped through one.

Visibility (contract): trade date ``T`` is usable from session ``T+1``; FINRA keeps no revision
history, so a file is the version as of our download (manifest sha256).
"""

from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import finra_fetch as F

RULE = "sv-ticker-on-trade-date-v2"
FIRST_CNMS = dt.date(2018, 8, 1)
DEFAULT_END = dt.date(2026, 9, 18)
URL = "https://cdn.finra.org/equity/regsho/daily/CNMSshvol{d:%Y%m%d}.txt"


def _manifest_path() -> Path:
    return F.SV_RAW_DIR / "manifest.csv"


def _file_name(d: dt.date) -> str:
    return f"CNMSshvol{d:%Y%m%d}.txt.gz"


def weekdays(start: dt.date, end: dt.date) -> list[dt.date]:
    out, d = [], start
    while d <= end:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


# ---------------------------------------------------------------- download
def download(start: dt.date, end: dt.date, retry_absent: bool = False, limit: int | None = None) -> dict[str, Any]:
    F.SV_RAW_DIR.mkdir(parents=True, exist_ok=True)
    man_path = _manifest_path()
    man = F.load_manifest(man_path)
    todo = [d for d in weekdays(start, end) if not F.is_done(man.get(d.isoformat()), F.SV_RAW_DIR, retry_absent)]
    if limit is not None:
        todo = todo[:limit]
    print(f"download: {len(todo)} weekday(s) to fetch in {start}..{end}", flush=True)
    stats: dict[str, float] = {"fetched": 0, "absent": 0, "bytes": 0}
    t0 = time.perf_counter()
    for i, d in enumerate(todo, 1):
        row = F.fetch_one(d, URL.format(d=d), F.SV_RAW_DIR / _file_name(d), man_path)
        if int(row["http_status"]) == 200:
            stats["fetched"] += 1
            stats["bytes"] += int(row["bytes"])
        else:
            stats["absent"] += 1
            print(f"  absent {d} http {row['http_status']}", flush=True)
        if i % 50 == 0 or i == len(todo):
            print(f"  {i}/{len(todo)} last={d} fetched={stats['fetched']} absent={stats['absent']} "
                  f"elapsed={time.perf_counter() - t0:.0f}s", flush=True)
    stats["elapsed_s"] = round(time.perf_counter() - t0, 1)
    return stats


# ---------------------------------------------------------------- parse
def parse_file(body: bytes, expect: dt.date) -> tuple[list[tuple], dict[str, int]]:
    """Rows ``(trade_date, symbol, short, exempt, total, market)``; trailer/blank lines ignored."""
    rows: list[tuple] = []
    st = {"lines": 0, "header": 0, "trailer_or_blank": 0, "bad": 0, "date_mismatch": 0}
    for raw in io.StringIO(body.decode("utf-8", errors="replace")):
        st["lines"] += 1
        line = raw.rstrip("\r\n")
        if not line.strip() or "|" not in line:
            st["trailer_or_blank"] += 1
            continue
        parts = line.split("|")
        if parts[0] == "Date":
            st["header"] += 1
            continue
        if len(parts) != 6 or not parts[0].isdigit():
            st["bad"] += 1
            continue
        try:
            d = dt.date(int(parts[0][:4]), int(parts[0][4:6]), int(parts[0][6:8]))
            sv, se, tv = float(parts[2]), float(parts[3]), float(parts[4])
        except ValueError:
            st["bad"] += 1
            continue
        if d != expect:
            st["date_mismatch"] += 1
        rows.append((d, parts[1].strip(), sv, se, tv, parts[5].strip()))
    return rows, st


def _tmp_dir() -> Path:
    p = C.build_root() / "_tmp" / "finra_sv"
    p.mkdir(parents=True, exist_ok=True)
    return p


def stage_raw_year(year: int, man: dict[str, dict[str, str]], receipt: dict[str, Any]) -> Path | None:
    """Verify + parse every landed file of ``year`` into one tmp parquet (resumable)."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    dest = _tmp_dir() / f"raw_{year}.parquet"
    rows_meta = sorted((k, v) for k, v in man.items() if k.startswith(str(year)) and int(v.get("http_status") or 0) == 200)
    ident = [(k, v["sha256_of_raw_bytes"]) for k, v in rows_meta]
    stamp = dest.with_suffix(".json")
    if dest.exists() and stamp.exists() and C.read_json(stamp).get("files") == ident:
        receipt.setdefault("parse", {})[str(year)] = C.read_json(stamp)["stats"] | {"reused": True}
        return dest
    if not rows_meta:
        return None
    schema = pa.schema([
        ("trade_date", pa.date32()), ("symbol", pa.string()), ("short_volume", pa.float64()),
        ("short_exempt_volume", pa.float64()), ("total_volume", pa.float64()), ("market", pa.string()),
    ])
    agg = {"files": 0, "rows": 0, "bad": 0, "date_mismatch": 0, "trailer_or_blank": 0, "row_count_vs_manifest_mismatch": 0}
    tmp = dest.with_name(dest.name + ".partial")
    with pq.ParquetWriter(tmp, schema, compression="zstd") as writer:
        for k, v in rows_meta:
            body = F.read_gzip_verified(F.SV_RAW_DIR / v["file"], v["sha256_of_raw_bytes"])
            rows, st = parse_file(body, dt.date.fromisoformat(k))
            agg["files"] += 1
            agg["rows"] += len(rows)
            for key in ("bad", "date_mismatch", "trailer_or_blank"):
                agg[key] += st[key]
            if len(rows) != int(v["rows"]):
                agg["row_count_vs_manifest_mismatch"] += 1
            if rows:
                cols = list(zip(*rows))
                writer.write_table(pa.table([pa.array(c, t.type) for c, t in zip(cols, schema)], schema=schema))
    tmp.replace(dest)
    C.write_json_atomic(stamp, {"files": ident, "stats": agg})
    receipt.setdefault("parse", {})[str(year)] = agg
    return dest


def map_year(con, raw: Path, th: Path, dest: Path) -> dict[str, Any]:
    canon_f = F.CANON_SQL.format(x="f.symbol")
    sql = f"""
        WITH th AS (
            SELECT map_date AS trade_date, security_id, trim(ticker_tk) AS tk,
                   {F.CANON_SQL.format(x='ticker_tk')} AS ck, sid_repaired
            FROM read_parquet('{th.as_posix()}')
        ),
        th_exact AS (   -- exact ticker -> id, unique per day
            SELECT trade_date, tk, min(security_id) AS sid, count(DISTINCT security_id) AS n,
                   bool_and(sid_repaired) AS rep
            FROM th GROUP BY 1, 2
        ),
        th_canon AS (
            SELECT trade_date, ck, min(security_id) AS sid, count(DISTINCT security_id) AS n,
                   bool_and(sid_repaired) AS rep
            FROM th GROUP BY 1, 2
        ),
        f AS (
            SELECT *, row_number() OVER (PARTITION BY trade_date, symbol ORDER BY total_volume DESC, short_volume DESC, market) AS dup_rank
            FROM read_parquet('{raw.as_posix()}')
        ),
        cand AS (
            SELECT f.trade_date, f.symbol, f.short_volume, f.short_exempt_volume, f.total_volume, f.market, f.dup_rank,
                   CASE WHEN e.n = 1 THEN e.sid WHEN e.n IS NULL AND c.n = 1 THEN c.sid END AS sid0,
                   coalesce(CASE WHEN e.n = 1 THEN e.rep WHEN e.n IS NULL AND c.n = 1 THEN c.rep END, false) AS rep0,
                   CASE WHEN e.n = 1 THEN 'exact' WHEN e.n > 1 THEN 'ambiguous'
                        WHEN c.n = 1 THEN 'canonical' WHEN c.n > 1 THEN 'ambiguous' ELSE 'unmapped' END AS basis0
            FROM f
            LEFT JOIN th_exact e ON e.trade_date = f.trade_date AND e.tk = trim(f.symbol)
            LEFT JOIN th_canon c ON c.trade_date = f.trade_date AND c.ck = {canon_f}
            WHERE f.dup_rank = 1
        ),
        ranked AS (
            SELECT *, count(*) OVER (PARTITION BY trade_date, sid0) AS n_sym,
                   count(*) FILTER (WHERE basis0 = 'exact') OVER (PARTITION BY trade_date, sid0) AS n_exact
            FROM cand
        )
        SELECT CAST(CASE WHEN sid0 IS NULL THEN NULL
                         WHEN n_sym = 1 THEN sid0
                         WHEN basis0 = 'exact' AND n_exact = 1 THEN sid0 END AS BIGINT) AS security_id,
               symbol, trade_date, short_volume, short_exempt_volume, total_volume, market,
               CASE WHEN sid0 IS NULL THEN basis0
                    WHEN n_sym = 1 OR (basis0 = 'exact' AND n_exact = 1) THEN basis0
                    ELSE 'collision' END AS map_basis,
               rep0 AND sid0 IS NOT NULL AND (n_sym = 1 OR (basis0 = 'exact' AND n_exact = 1)) AS sid_repaired
        FROM ranked
        ORDER BY trade_date, symbol
    """
    n = C.copy_to_parquet(con, sql, dest)
    dups = con.execute(
        f"SELECT count(*) FROM (SELECT trade_date, symbol FROM read_parquet('{raw.as_posix()}') GROUP BY 1, 2 HAVING count(*) > 1)"
    ).fetchone()[0]
    basis = dict(con.execute(f"SELECT map_basis, count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1").fetchall())
    ids = con.execute(
        f"SELECT count(DISTINCT security_id), count(*) FILTER (WHERE security_id IS NOT NULL), "
        f"sum(total_volume) FILTER (WHERE security_id IS NOT NULL) / nullif(sum(total_volume), 0), "
        f"count(DISTINCT trade_date), min(trade_date), max(trade_date), count(*) FILTER (WHERE sid_repaired) "
        f"FROM read_parquet('{dest.as_posix()}')"
    ).fetchone()
    return {
        "rows": n, "duplicate_symbol_keys_resolved": int(dups),
        "map_basis": {k: int(v) for k, v in basis.items()},
        "ids": int(ids[0]), "mapped_rows": int(ids[1]),
        "mapped_share_rows": round(ids[1] / n, 4) if n else None,
        "mapped_share_total_volume": round(float(ids[2]), 4) if ids[2] is not None else None,
        "trade_dates": int(ids[3]), "first": str(ids[4]), "last": str(ids[5]), "sid0_repaired_rows": int(ids[6]),
    }


def build(start: dt.date, end: dt.date) -> dict[str, Any]:
    t_all = time.perf_counter()
    receipt: dict[str, Any] = {"stage": "short_volume", "rule": RULE, "window": [start.isoformat(), end.isoformat()],
                               "tickerhistory": C.file_identity(C.TICKERHISTORY)}
    man_path = _manifest_path()
    man = F.load_manifest(man_path)
    receipt["raw_manifest"] = {"path": str(man_path), "sha256": C.sha256_file(man_path), "dates": len(man)}
    con = C.connect(memory=F.DUCKDB_MEMORY, threads=2, temp_dir=_tmp_dir() / "spill")
    with C.timed(receipt, "calendar"):
        sessions = F.sessions(con)
    receipt["calendar"] = {"rule": f"vendor tradingDate with >= {C.MIN_ROWS_PER_SESSION} rows",
                           "first": sessions[0].isoformat(), "last": sessions[-1].isoformat()}
    with C.timed(receipt, "sid0_repair"):
        _, receipt["sid0_repair"] = F.sid0_repairs(con)
    out = C.stage_dir("short_volume")
    years: dict[str, Any] = {}
    for y in C.years_between(start, end):
        ys, ye = max(start, dt.date(y, 1, 1)), min(end, dt.date(y, 12, 31))
        cal = [d for d in sessions if ys <= d <= ye]
        landed = {dt.date.fromisoformat(k) for k, v in man.items() if int(v.get("http_status") or 0) == 200}
        absent = {dt.date.fromisoformat(k) for k, v in man.items() if int(v.get("http_status") or 0) in F.ABSENT_CODES}
        cov: dict[str, Any] = {
            "sessions": len(cal),
            "landed": sum(1 for d in cal if d in landed),
            "missing_sessions": [d.isoformat() for d in cal if d not in landed],
            "missing_sessions_http_absent": [d.isoformat() for d in cal if d in absent],
            "files_on_non_sessions": [d.isoformat() for d in sorted(landed) if ys <= d <= ye and d not in set(cal)],
            "absent_weekdays": len([d for d in absent if ys <= d <= ye]),
        }
        with C.timed(receipt, f"parse_{y}"):
            raw = stage_raw_year(y, man, receipt)
        if raw is None:
            years[str(y)] = {"coverage": cov, "rows": 0}
            continue
        with C.timed(receipt, f"tickers_{y}"):
            th = _tmp_dir() / f"th_{y}.parquet"
            th_dates = sorted({d for d in sessions if ys <= d <= ye} | {dt.date.fromisoformat(k) for k in man if k.startswith(str(y))})
            receipt.setdefault("ticker_extract_rows", {})[str(y)] = F.ticker_extract(con, th_dates, th)
        with C.timed(receipt, f"map_{y}"):
            res = map_year(con, raw, th, out / f"year={y}" / "short_volume.parquet")
        res["coverage"] = cov
        years[str(y)] = res
        print(f"year {y}: rows={res['rows']} mapped={res['mapped_share_rows']} sessions={cov['sessions']} "
              f"landed={cov['landed']} missing={len(cov['missing_sessions'])}", flush=True)
    receipt["years"] = years
    receipt["rows_total"] = sum(v.get("rows", 0) for v in years.values())
    receipt["timings_s"]["total"] = round(time.perf_counter() - t_all, 1)
    receipt["output_files"] = {
        y: C.file_identity(out / f"year={y}" / "short_volume.parquet")
        for y in years if (out / f"year={y}" / "short_volume.parquet").exists()
    }
    C.write_json_atomic(out / "manifest.json", receipt)
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("download")
    d.add_argument("--start", type=dt.date.fromisoformat, default=FIRST_CNMS)
    d.add_argument("--end", type=dt.date.fromisoformat, default=DEFAULT_END)
    d.add_argument("--retry-absent", action="store_true")
    d.add_argument("--limit", type=int)
    b = sub.add_parser("build")
    b.add_argument("--start", type=dt.date.fromisoformat, default=FIRST_CNMS)
    b.add_argument("--end", type=dt.date.fromisoformat, default=DEFAULT_END)
    args = ap.parse_args(argv)
    if args.cmd == "download":
        print(json.dumps(download(args.start, args.end, args.retry_absent, args.limit)), flush=True)
    else:
        rec = build(args.start, args.end)
        print(json.dumps({k: v for k, v in rec.items() if k not in ("years", "parse", "output_files")}, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
