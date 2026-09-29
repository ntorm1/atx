"""Stage P: vendor daily bars -> repaired, point-in-time price panel.

Source: ORATS TickerHistory3 (``TICKERHISTORY``). Output (see docs/ALPHA_PANEL.md):
``calendar.parquet`` and ``prices/year=YYYY/prices.parquet``.

Vendor semantics (verified on AAPL 2020-08-31 split, 2021-01-04, 2026-08-10 dividend):
``close`` is the raw traded close, ``closeUnadjPr`` the prior raw close, ``returnFactor`` the
adjustment applied to the prior close on the row's date (0.25 on a 4:1 split, 0.9991 on an
ex-dividend day), ``closePr = closeUnadjPr * returnFactor`` and
``totalReturn = close / closePr - 1``.

Rule ``factor-break-v1`` (same parameters as the atx-impl role repair, ruling C-35): the vendor
factor is not chained across 2021-01-04, so on a *mass session* (>= 50 jump cells) a factor
step that no corporate action made is removed. With step s = -ln(returnFactor) (the change of the
cumulative factor), r = ln(close / prior raw close):
* jump cell: |s| > 0.01 and |r + s| > |r| + 0.01; mass session: >= 50 jump cells;
* on a mass session every step with |s| > 1e-9 is kept_gap (prior observation > 10 calendar days
  back), kept_split_follow (s < 0 and r >= max(|s|/2, ln 1.25), or s > 0 and -r >= max(s/2,
  ln 1.25)), kept_distribution (0 < s < ln 1.25), else repaired: the day's return becomes the raw
  close ratio (the spurious factor is dropped).
Every decision reads rows dated <= the session only.

``adj_close`` chains ``1 + ret`` backward from the line's last raw close in the build window, so
ratios of ``adj_close`` are total-return ratios. ``ret_guarded`` replicates the atx-impl
``rough_return`` guard used by ``mkt_ret``: non-finite, |ln(1+ret)| > 1.5, or
|ln(1+ret)| > |ln raw ratio| + 0.10.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import json
import math
import shutil
import sys
from pathlib import Path
from typing import Any

from . import common as C

RULE = "factor-break-v1"
BUCKETS = 16
CELL_STEP = 0.01
CELL_EXCESS = 0.01
MASS_MIN_CELLS = 50
NOISE = 1e-9
SPLIT_RATIO = 1.25
MAX_GAP_DAYS = 10
IV_TENORS = (5, 10, 21, 42, 63, 126, 252)
IV_DOMAIN = (0.02, 5.0)
SHARES_ROW_CEILING = 100_000_000  # vendor thousands; above this the row is a units defect (C-81)
PROJECT_START = dt.date.fromisoformat(os.environ.get("ATX_PRICES_START", "2017-10-01"))  # a quarter before WARMUP_START so the first returns have a prior bar
# history builds (ATX_PRICES_START set, separate ATX_ALPHA_PANEL_ROOT) write every year from the start
WRITE_FROM = PROJECT_START if "ATX_PRICES_START" in os.environ else C.WARMUP_START


def _proj_dir() -> Path:
    return C.build_root() / "_tmp" / "prices_proj"


def build_calendar(con, receipt: dict[str, Any]) -> None:
    src = C.TICKERHISTORY.as_posix()
    sql = f"""
        SELECT tradingDate AS session_date, count(*) AS vendor_rows,
               count(DISTINCT securityID) AS vendor_ids,
               count(*) >= {C.MIN_ROWS_PER_SESSION} AS is_session
        FROM read_parquet('{src}') GROUP BY 1 ORDER BY 1
    """
    tmp = C.build_root() / "_tmp" / "calendar_all.parquet"
    C.copy_to_parquet(con, sql, tmp)
    rows = C.copy_to_parquet(
        con,
        f"SELECT session_date, vendor_rows, vendor_ids FROM read_parquet('{tmp.as_posix()}') WHERE is_session",
        C.calendar_path(),
    )
    dropped = con.execute(
        f"SELECT count(*), sum(vendor_rows) FROM read_parquet('{tmp.as_posix()}') WHERE NOT is_session"
    ).fetchone()
    receipt["calendar"] = {"sessions": rows, "non_session_dates": int(dropped[0]), "non_session_rows": int(dropped[1] or 0)}


def project(con, receipt: dict[str, Any]) -> None:
    """One scan of the vendor file into BUCKETS hive partitions (by securityID % BUCKETS)."""
    out = _proj_dir()
    done = out / "_SUCCESS"
    ident = json.dumps(C.file_identity(C.TICKERHISTORY), sort_keys=True)
    if done.exists() and done.read_text(encoding="utf-8") == ident:
        receipt["project"] = "reused"
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    src = C.TICKERHISTORY.as_posix()
    iv_cols = ", ".join(f"atmCenI_{t}d AS iv_raw_{t}d" for t in IV_TENORS)
    sql = f"""
        SELECT tradingDate AS session_date, securityID AS security_id, ticker_tk AS ticker,
               CAST(open AS DOUBLE) AS open, CAST(high AS DOUBLE) AS high, CAST(low AS DOUBLE) AS low,
               CAST(close AS DOUBLE) AS close, CAST(closeUnadjPr AS DOUBLE) AS prev_raw_close_vendor,
               CAST(returnFactor AS DOUBLE) AS return_factor, CAST(totalReturn AS DOUBLE) AS total_return_vendor,
               CAST(cumulReturnFactor AS DOUBLE) AS cumul_factor, volume, shares AS shares_thousands,
               earnFlag AS earn_flag, GICS AS gics, {iv_cols},
               CAST(securityID % {BUCKETS} AS INTEGER) AS bucket
        FROM read_parquet('{src}')
        WHERE tradingDate >= DATE '{PROJECT_START}' AND securityID IS NOT NULL AND tradingDate IS NOT NULL
    """
    con.execute(
        f"COPY ({sql}) TO '{out.as_posix()}' (FORMAT PARQUET, COMPRESSION ZSTD, PARTITION_BY (bucket), "
        f"ROW_GROUP_SIZE 131072)"
    )
    done.write_text(ident, encoding="utf-8")
    shutil.rmtree(_proj_dir() / "_repaired", ignore_errors=True)
    receipt["project"] = {"rows": int(con.execute(f"SELECT count(*) FROM read_parquet('{out.as_posix()}/*/*.parquet')").fetchone()[0])}


REPAIR_MAX_BRACKET_DAYS = 400


def repair_sid0(con, receipt: dict[str, Any]) -> None:
    """Reassign vendor rows filed under the catch-all securityID 0 to their real line.

    Rule sid0-bracket-v1: a sid-0 row (d, ticker) goes to line S iff it is the only sid-0 row with
    that ticker on d, and the nearest non-zero rows carrying the ticker strictly before and strictly
    after d both belong to S, each within 400 calendar days. The original row wins a key collision.
    """
    out = _proj_dir() / "_repaired"
    done = out / "_SUCCESS"
    if done.exists():
        receipt["sid0_repair"] = C.read_json(out / "stats.json")
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    glob = (_proj_dir() / "bucket=*" / "*.parquet").as_posix()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE nz AS
        SELECT DISTINCT ticker, session_date, security_id FROM read_parquet('{glob}', hive_partitioning = false)
        WHERE security_id > 0 AND ticker IS NOT NULL
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE z AS
        SELECT * EXCLUDE (bucket) FROM (
            SELECT *, count(*) OVER (PARTITION BY session_date, ticker) AS n_same
            FROM read_parquet('{(_proj_dir() / "bucket=0" / "*.parquet").as_posix()}', hive_partitioning = true)
            WHERE security_id = 0 AND ticker IS NOT NULL)
        WHERE n_same = 1
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE nz_dedup AS
        SELECT ticker, session_date, min(security_id) AS sid, count(*) AS n FROM nz GROUP BY 1, 2
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE zb AS
        SELECT z.*, p.sid AS prev_sid, p.session_date AS prev_date, p.n AS prev_n
        FROM z ASOF LEFT JOIN nz_dedup p ON z.ticker = p.ticker AND z.session_date > p.session_date
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE zbb AS
        SELECT zb.*, q.sid AS next_sid, q.session_date AS next_date, q.n AS next_n
        FROM zb ASOF LEFT JOIN (SELECT ticker, -epoch(session_date) AS neg_t, session_date, sid, n FROM nz_dedup) q
          ON zb.ticker = q.ticker AND -epoch(zb.session_date) > q.neg_t
    """)
    ok = f"""prev_sid IS NOT NULL AND prev_sid = next_sid AND prev_n = 1 AND next_n = 1
             AND date_diff('day', prev_date, session_date) <= {REPAIR_MAX_BRACKET_DAYS}
             AND date_diff('day', session_date, next_date) <= {REPAIR_MAX_BRACKET_DAYS}"""
    stats = con.execute(f"""
        SELECT count(*), count(*) FILTER (WHERE {ok}), count(DISTINCT ticker) FILTER (WHERE {ok}),
               count(DISTINCT prev_sid) FILTER (WHERE {ok})
        FROM zbb""").fetchone()
    cols = [r[0] for r in con.execute("DESCRIBE z").fetchall() if r[0] not in ("security_id", "n_same")]
    for b in range(BUCKETS):
        dest = out / f"bucket_{b:02d}.parquet"
        C.copy_to_parquet(con, f"""
            SELECT prev_sid AS security_id, {', '.join(cols)}, true AS repaired
            FROM zbb WHERE {ok} AND prev_sid % {BUCKETS} = {b}""", dest)
    top = con.execute(f"""
        SELECT ticker, prev_sid, count(*) c, min(session_date), max(session_date) FROM zbb WHERE {ok}
        GROUP BY 1, 2 ORDER BY c DESC LIMIT 15""").fetchall()
    st = {"rule": "sid0-bracket-v1", "sid0_unique_rows": int(stats[0]), "repaired_rows": int(stats[1]),
          "repaired_tickers": int(stats[2]), "repaired_lines": int(stats[3]),
          "top": [[t, int(s), int(c), str(a), str(z)] for t, s, c, a, z in top]}
    C.write_json_atomic(out / "stats.json", st)
    done.write_text("ok", encoding="utf-8")
    receipt["sid0_repair"] = st


def _bucket_glob(b: int) -> str:
    return (_proj_dir() / f"bucket={b}" / "*.parquet").as_posix()


def _bucket_sources(b: int) -> str:
    """Projection rows of bucket ``b`` plus sid-0 rows repaired into it (``repaired`` true)."""
    orig = _bucket_glob(b)
    rep = (_proj_dir() / "_repaired" / f"bucket_{b:02d}.parquet").as_posix()
    return (f"(SELECT * EXCLUDE (bucket), false AS repaired FROM read_parquet('{orig}', hive_partitioning = true) "
            f"UNION ALL BY NAME SELECT * FROM read_parquet('{rep}'))")


def _steps_sql(b: int) -> str:
    """Unique-key observations of bucket ``b`` with the per-line step quantities."""
    cal = C.calendar_path().as_posix()
    return f"""
        WITH raw AS (
            SELECT * FROM {_bucket_sources(b)}
            WHERE session_date IN (SELECT session_date FROM read_parquet('{cal}'))
              AND security_id > 0  -- securityID 0 is the vendor's catch-all; see repair_sid0
        ),
        keyed AS (
            SELECT *,
                   count(*) FILTER (WHERE NOT repaired) OVER (PARTITION BY security_id, session_date) AS key_orig,
                   count(*) FILTER (WHERE repaired) OVER (PARTITION BY security_id, session_date) AS key_rep
            FROM raw
        ),
        obs AS (
            SELECT * EXCLUDE (key_orig, key_rep) FROM keyed
            WHERE (NOT repaired AND key_orig = 1) OR (repaired AND key_orig = 0 AND key_rep = 1)
        ),
        lagged AS (
            SELECT *,
                   lag(close) OVER w AS prev_close, lag(session_date) OVER w AS prev_date
            FROM obs
            WHERE close > 0 AND isfinite(close)
            WINDOW w AS (PARTITION BY security_id ORDER BY session_date)
        )
        SELECT *,
               CASE WHEN return_factor > 0 AND isfinite(return_factor) THEN -ln(return_factor) END AS s,
               CASE WHEN prev_close > 0 THEN ln(close / prev_close) END AS r,
               date_diff('day', prev_date, session_date) AS gap_days
        FROM lagged
    """


def jump_counts(con, receipt: dict[str, Any]) -> list[dt.date]:
    parts = []
    dup_total = 0
    for b in range(BUCKETS):
        rows = con.execute(
            f"""
            SELECT session_date, count(*) FILTER (WHERE abs(s) > {CELL_STEP} AND abs(r + s) > abs(r) + {CELL_EXCESS})
            FROM ({_steps_sql(b)}) GROUP BY 1
            """
        ).fetchall()
        parts.extend(rows)
        dup_total += con.execute(
            f"""SELECT count(*) FROM (SELECT security_id, session_date, count(*) c FROM read_parquet('{_bucket_glob(b)}')
                GROUP BY 1, 2 HAVING c > 1)"""
        ).fetchone()[0]
    per_day: dict[dt.date, int] = {}
    for d, n in parts:
        per_day[d] = per_day.get(d, 0) + int(n)
    mass = sorted(d for d, n in per_day.items() if n >= MASS_MIN_CELLS)
    quiet = max((n for d, n in per_day.items() if n < MASS_MIN_CELLS), default=0)
    receipt["factor_break"] = {
        "rule": RULE,
        "mass_sessions": [{"session": d.isoformat(), "jump_cells": per_day[d]} for d in mass],
        "max_non_mass_jump_cells": quiet,
        "duplicate_keys_quarantined": int(dup_total),
    }
    return mass


def finalize_bucket(con, b: int, mass: list[dt.date], dest: Path) -> dict[str, Any]:
    in_mass = ("session_date IN (" + ", ".join(f"DATE '{d}'" for d in mass) + ")") if mass else "FALSE"
    ln_split = math.log(SPLIT_RATIO)
    iv_sel = ", ".join(
        f"CASE WHEN iv_raw_{t}d >= {IV_DOMAIN[0]} AND iv_raw_{t}d <= {IV_DOMAIN[1]} THEN CAST(iv_raw_{t}d AS DOUBLE) END "
        f"AS iv_atm_{t}d"
        for t in IV_TENORS
    )
    sql = f"""
        WITH st AS ({_steps_sql(b)}),
        cls AS (
            SELECT *,
                CASE
                    WHEN NOT ({in_mass}) OR s IS NULL OR abs(s) <= {NOISE} OR prev_close IS NULL THEN 'none'
                    WHEN gap_days > {MAX_GAP_DAYS} THEN 'kept_gap'
                    WHEN (s < 0 AND r >= greatest(abs(s) / 2, {ln_split})) OR (s > 0 AND -r >= greatest(s / 2, {ln_split}))
                        THEN 'kept_split_follow'
                    WHEN s > 0 AND s < {ln_split} THEN 'kept_distribution'
                    ELSE 'repaired'
                END AS fb_action
            FROM st
        ),
        src AS (
            SELECT *,
                CASE WHEN prev_close IS NULL THEN NULL
                     WHEN fb_action = 'repaired' THEN 'repaired_raw'
                     WHEN prev_raw_close_vendor > 0 AND abs(prev_raw_close_vendor / prev_close - 1) <= 0.001
                          AND isfinite(total_return_vendor) AND total_return_vendor > -1 THEN 'vendor'
                     ELSE 'observed' END AS ret_source
            FROM cls
        ),
        rets AS (
            SELECT *,
                CASE WHEN ret_source = 'repaired_raw' THEN close / prev_close - 1
                     WHEN ret_source = 'vendor' THEN total_return_vendor
                     WHEN ret_source = 'observed' THEN
                          close / (prev_close * CASE WHEN return_factor > 0 AND isfinite(return_factor) THEN return_factor ELSE 1 END) - 1
                END AS ret_all
            FROM src
        ),
        chained AS (
            SELECT *,
                sum(CASE WHEN prev_close IS NULL THEN 0 ELSE ln(1 + ret_all) END)
                    OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS cum_log,
                last_value(close) OVER (PARTITION BY security_id ORDER BY session_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS anchor_close,
                sum(CASE WHEN prev_close IS NULL THEN 0 ELSE ln(1 + ret_all) END)
                    OVER (PARTITION BY security_id) AS total_log
            FROM rets
        )
        SELECT session_date, security_id, ticker, open, high, low, close,
               CASE WHEN prev_close IS NULL THEN NULL ELSE ret_all END AS ret,
               CASE WHEN prev_close IS NULL THEN NULL
                    ELSE (NOT isfinite(ln(1 + ret_all)) OR abs(ln(1 + ret_all)) > 1.5 OR abs(ln(1 + ret_all)) > abs(r) + 0.10)
               END AS ret_guarded,
               anchor_close * exp(cum_log - total_log) AS adj_close,
               prev_close AS prev_raw_close, gap_days,
               volume, close * volume AS dollar_volume,
               CASE WHEN shares_thousands > 0 AND shares_thousands <= {SHARES_ROW_CEILING}
                    THEN CAST(shares_thousands AS DOUBLE) * 1000.0 END AS shares_vendor,
               shares_thousands > {SHARES_ROW_CEILING} AS shares_above_ceiling,
               return_factor, cumul_factor, total_return_vendor, fb_action, ret_source, repaired AS sid0_repaired,
               earn_flag, NULLIF(gics, '') AS gics, {iv_sel}
        FROM chained
        WHERE session_date >= DATE '{PROJECT_START}'
    """
    rows = C.copy_to_parquet(con, sql, dest)
    stats = con.execute(
        f"SELECT fb_action, count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1"
    ).fetchall()
    return {"rows": rows, "fb_actions": {k: int(v) for k, v in stats}}


def write_years(con, receipt: dict[str, Any]) -> None:
    fin = (C.build_root() / "_tmp" / "prices_final").as_posix()
    out = C.stage_dir("prices")
    years = {}
    first, last = con.execute(f"SELECT min(session_date), max(session_date) FROM read_parquet('{fin}/*.parquet')").fetchone()
    for y in range(max(WRITE_FROM.year, first.year), last.year + 1):
        dest = out / f"year={y}" / "prices.parquet"
        sql = f"""
            SELECT * FROM read_parquet('{fin}/*.parquet')
            WHERE session_date >= DATE '{max(dt.date(y, 1, 1), WRITE_FROM)}' AND session_date <= DATE '{y}-12-31'
            ORDER BY session_date, security_id
        """
        years[str(y)] = C.copy_to_parquet(con, sql, dest)
    receipt["years"] = years
    receipt["last_session"] = last.isoformat()


def run(stages: list[str]) -> dict[str, Any]:
    receipt: dict[str, Any] = {"stage": "prices", "rule": RULE, "source": C.file_identity(C.TICKERHISTORY)}
    con = C.connect(memory="600MB", threads=2)
    if "calendar" in stages:
        with C.timed(receipt, "calendar"):
            build_calendar(con, receipt)
    if "project" in stages:
        with C.timed(receipt, "project"):
            project(con, receipt)
    if "repair" in stages:
        with C.timed(receipt, "repair"):
            repair_sid0(con, receipt)
            print(receipt["sid0_repair"], flush=True)
    if "finalize" in stages:
        if "sid0_repair" not in receipt:
            receipt["sid0_repair"] = C.read_json(_proj_dir() / "_repaired" / "stats.json")
        with C.timed(receipt, "jump_counts"):
            mass = jump_counts(con, receipt)
        fin = C.build_root() / "_tmp" / "prices_final"
        fin.mkdir(parents=True, exist_ok=True)
        buckets = {}
        with C.timed(receipt, "finalize_buckets"):
            for b in range(BUCKETS):
                buckets[str(b)] = finalize_bucket(con, b, mass, fin / f"bucket_{b:02d}.parquet")
                print(f"bucket {b}: {buckets[str(b)]}", flush=True)
        receipt["buckets"] = buckets
        with C.timed(receipt, "write_years"):
            write_years(con, receipt)
        C.write_json_atomic(C.stage_dir("prices") / "manifest.json", receipt)
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stages", default="calendar,project,repair,finalize")
    args = ap.parse_args(argv)
    receipt = run([s.strip() for s in args.stages.split(",") if s.strip()])
    print({k: v for k, v in receipt.items() if k not in ("buckets",)}, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
