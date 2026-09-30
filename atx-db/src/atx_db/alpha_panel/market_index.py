"""Stage ``market_index`` (S3 market returns): CRSP-style value- and equal-weight index returns, plus the S3 exit
checks against the French library -> ``market/index_returns.parquet`` and ``validation/factors_market.json``.

Universes, per session, from the panel (read only):

* ``member_common``: ``member_equity`` and a CRSP share-code 10/11-like common stock: ``security_type`` common or
  common_unverified, not an ADR (``is_adr`` / ``is_adr_likely``), foreign private issuer (``is_fpi``), REIT,
  LP or royalty trust.
* ``all_common``: the same stock filters on every ``is_operating`` line listed on NYSE, NYSE American, Nasdaq,
  NYSE Arca or Cboe BZX (``exchange``), without the top-3,000 liquidity screen (closer to the CRSP universe).

Rule ``crsp-index-v1``: market equity ME = ``market/shares_daily.shrout`` x raw close. Daily: the return of session d
averages ``ret`` over the lines in the universe at the previous session with a positive ME there and an unguarded
return at d, weighted by that lagged ME (``VW``) or equally (``EW``). Monthly: a line's month return compounds its
daily returns in the month (a line with a guarded day is left out of that month); weights are ME at the last
session of the previous month over the lines in the universe then. Delisting returns are not added (the delisting
stage's are imputed). ``available_at`` = 22:00 UTC of the period's last session.

Exit checks (ruling D6: windows end 2022-12-31; ``--end`` cannot be later):

* Mkt-RF: monthly and daily correlation of ``VW - rf`` with French Mkt-RF; ``rf`` = the H.15 3-month bill
  (DTB3) of the last observation before the period, per month /1200 and per session /36000 x calendar days.
* SMB and HML from our own 2x3 sorts (rule ``ff-2x3-v1``, French's construction): at the end of June t, firms
  (CIK; ME summed over the issuer's lines; the issuer's primary line carries its return) in ``all_common`` with ME
  in June t and December t-1 and positive book equity are sorted on June ME at the NYSE median (``exchange`` XNYS)
  and on BE/ME at the NYSE 30th / 70th percentiles; BE = the fundamentals stage ``be`` of the latest filing event
  known by the end of June t whose period is a fiscal year ending in calendar t-1 (else the latest ``be`` known
  then), ME = December t-1 (the first 2018 session stands in for December 2017, before the panel). Six value-weight
  portfolios (weights: firm ME at the end of the previous month) are held July t to June t+1; SMB = mean(small) -
  mean(big), HML = mean(SH, BH) - mean(SL, BL). Correlation with French SMB / HML (FF3, monthly).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
from typing import Any

from . import common as C
from . import market_common as M

STAGE = "market_index"
SCHEMA = "atx.alpha-panel.market-index/v1"
RULE = "crsp-index-v1"
MODULES = ("market_index", "market_common", "common")
OUT_DIR = "market"
HOLDOUT_END = dt.date(2022, 12, 31)
LISTED = ("XNYS", "XNAS", "XASE", "ARCX", "BATS")
INPUTS = ("panel", "market/market_shares_manifest.json", "reference", "prices")
GATES = {"mkt_rf_monthly_rho": 0.99, "smb_monthly_rho": 0.90}

LAKE_STAGES = [
    {"name": "market_index", "lane": "MKT", "schema": "atx.alpha-panel.market-index/v1",
     "module": "atx_db.alpha_panel.market_index", "args": ["build"],
     "inputs": ["panel", "market_shares", "reference", "prices"],
     "manifest": "market/market_index_manifest.json",
     "outputs": [{"glob": "market/index_returns.parquet", "view": "market_index_returns"}],
     "staleness": "daily; rebuilt with the panel", "vintage": "event", "guard_gb": 0.5},
]


def common_filter(alias: str = "p") -> str:
    a = alias
    return (f"{a}.security_type IN ('common', 'common_unverified') AND NOT coalesce({a}.is_adr, false) "
            f"AND NOT coalesce({a}.is_adr_likely, false) AND NOT coalesce({a}.is_fpi, false) "
            f"AND NOT coalesce({a}.is_reit, false) AND NOT coalesce({a}.is_lp, false) "
            f"AND NOT coalesce({a}.is_royalty_trust, false)")


def load_universe(con, receipt: dict[str, Any]) -> None:
    """``u``: every all_common / member_common line-session with ME, return and sort inputs (disk-backed)."""
    panel = (C.build_root() / "panel" / "year=*" / "*.parquet").as_posix()
    shares = (C.build_root() / OUT_DIR / "shares_daily" / "year=*" / "shares_daily.parquet").as_posix()
    listed = ", ".join(f"'{x}'" for x in LISTED)
    con.execute(f"""
        CREATE OR REPLACE TABLE u AS
        SELECT p.session_date, p.security_id, p.cik, coalesce(p.is_issuer_primary, true) AS is_primary,
               p.exchange, p.raw_close, p.ret, coalesce(p.ret_guarded, true) AS ret_guarded,
               coalesce(p.member_equity, false) AS member_common, s.shrout, s.shrout * p.raw_close AS me
        FROM read_parquet('{panel}', union_by_name = true, hive_partitioning = false) p
        LEFT JOIN read_parquet('{shares}', hive_partitioning = false) s USING (session_date, security_id)
        WHERE {common_filter('p')} AND coalesce(p.is_operating, false) AND NOT coalesce(p.is_index, false)
          AND p.exchange IN ({listed})
    """)
    receipt["universe"] = [list(map(str, r)) for r in con.execute("""
        SELECT year(session_date), count(*), count(*) FILTER (WHERE member_common), count(me),
               count(*) FILTER (WHERE member_common AND me IS NOT NULL)
        FROM u GROUP BY 1 ORDER BY 1""").fetchall()]


def daily_sql() -> str:
    return f"""
    WITH cal AS {M.calendar_sql()},
    w AS (
        SELECT session_date, security_id, ret, ret_guarded,
               lag(session_date) OVER x AS d_lag, lag(me) OVER x AS me_lag, lag(member_common) OVER x AS mem_lag
        FROM u WINDOW x AS (PARTITION BY security_id ORDER BY session_date)
    ),
    pairs AS (
        SELECT w.* FROM w JOIN cal USING (session_date)
        WHERE w.d_lag = cal.prev_session AND w.me_lag > 0 AND w.ret IS NOT NULL AND NOT w.ret_guarded
    )
    SELECT 'D' AS freq, session_date AS period_end, universe,
           sum(me_lag * ret) / sum(me_lag) AS vw, avg(ret) AS ew, count(*) AS n_stocks, sum(me_lag) AS me_lag_total
    FROM (SELECT *, 'all_common' AS universe FROM pairs
          UNION ALL SELECT *, 'member_common' AS universe FROM pairs WHERE mem_lag)
    GROUP BY ALL
    """


def monthly_sql() -> str:
    return f"""
    WITH m AS (
        SELECT security_id, CAST(date_trunc('month', session_date) AS DATE) AS month,
               exp(sum(ln(1 + ret))) - 1 AS mret, bool_or(ret_guarded OR ret IS NULL) AS bad, count(*) AS n
        FROM u GROUP BY 1, 2
    ),
    last AS (
        SELECT *, CAST(date_trunc('month', session_date) + INTERVAL 1 MONTH AS DATE) AS next_month FROM u
        QUALIFY session_date = max(session_date) OVER (PARTITION BY date_trunc('month', session_date))
    ),
    j AS (
        SELECT m.month, m.security_id, m.mret, l.me AS me_lag, l.member_common AS mem_lag
        FROM m JOIN last l ON l.security_id = m.security_id AND l.next_month = m.month
        WHERE l.me > 0 AND NOT m.bad
    )
    SELECT 'M' AS freq, CAST(last_day(month) AS DATE) AS period_end, universe,
           sum(me_lag * mret) / sum(me_lag) AS vw, avg(mret) AS ew, count(*) AS n_stocks, sum(me_lag) AS me_lag_total
    FROM (SELECT *, 'all_common' AS universe FROM j UNION ALL SELECT *, 'member_common' AS universe FROM j WHERE mem_lag)
    GROUP BY ALL
    """


def build_index(con, receipt: dict[str, Any]) -> None:
    con.execute(f"CREATE OR REPLACE TABLE idx AS {daily_sql()} UNION ALL {monthly_sql()}")
    last_session = con.execute("SELECT max(session_date) FROM u").fetchone()[0]
    # the running month (last session more than 4 days before its month end) is left out
    out = C.stage_dir(OUT_DIR) / "index_returns.parquet"
    receipt["index_rows"] = C.copy_to_parquet(con, f"""
        WITH cal AS {M.calendar_sql()},
        month_end AS (SELECT CAST(date_trunc('month', session_date) AS DATE) AS m, max(session_date) AS last_session
                      FROM cal GROUP BY 1)
        SELECT i.freq, i.period_end, i.universe, w.weighting,
               CASE w.weighting WHEN 'VW' THEN i.vw ELSE i.ew END AS ret, i.n_stocks, i.me_lag_total,
               CAST(coalesce(me.last_session, i.period_end) AS TIMESTAMP) + {M.MARK} AS available_at
        FROM idx i CROSS JOIN (VALUES ('VW'), ('EW')) w(weighting)
        LEFT JOIN month_end me ON i.freq = 'M' AND me.m = CAST(date_trunc('month', i.period_end) AS DATE)
        WHERE i.freq = 'D' OR me.m < CAST(date_trunc('month', DATE '{last_session}') AS DATE)
           OR DATE '{last_session}' >= CAST(last_day(DATE '{last_session}') AS DATE) - 4
        ORDER BY freq, universe, weighting, period_end
    """, out)


# ---------------------------------------------------------------- factor replication
def ff_sorts_sql(end: dt.date) -> str:
    """Monthly 2x3 portfolio returns (size x BE/ME) with NYSE breakpoints, July 2018 .. ``end``."""
    ev = (C.build_root() / "fundamentals" / "events.parquet").as_posix()
    return f"""
    WITH firm AS (   -- firm ME and primary line per session
        SELECT session_date, cik, sum(me) AS fme, arg_max(security_id, CASE WHEN is_primary THEN me ELSE -1 END) AS pline,
               bool_or(exchange = 'XNYS' AND is_primary) AS nyse
        FROM u WHERE cik IS NOT NULL AND me > 0 GROUP BY 1, 2
    ),
    mlast AS (SELECT CAST(date_trunc('month', session_date) AS DATE) AS month, max(session_date) AS d FROM u GROUP BY 1),
    fm AS (SELECT f.*, ml.month FROM firm f JOIN mlast ml ON f.session_date = ml.d),
    first18 AS (SELECT min(session_date) AS d FROM u),
    june AS (SELECT fm.*, year(month) AS t FROM fm WHERE month(month) = 6 AND month <= DATE '{end}'),
    dec AS (
        SELECT cik, year(month) + 1 AS t, fme AS me_dec FROM fm WHERE month(month) = 12
        UNION ALL
        SELECT cik, 2018 AS t, fme FROM firm WHERE session_date = (SELECT d FROM first18)
    ),
    be_fy AS (
        SELECT j.cik, j.t, arg_max(e.be, e.clock_utc) AS be
        FROM june j JOIN read_parquet('{ev}') e ON e.cik = j.cik
        WHERE e.fiscal_period = 'FY' AND year(e.period_end) = j.t - 1 AND e.be IS NOT NULL
          AND e.clock_utc < CAST(j.session_date AS TIMESTAMP) + {M.MARK}
        GROUP BY 1, 2
    ),
    be_any AS (
        SELECT j.cik, j.t, arg_max(e.be, e.clock_utc) AS be
        FROM june j JOIN read_parquet('{ev}') e ON e.cik = j.cik
        WHERE e.be IS NOT NULL AND e.clock_utc < CAST(j.session_date AS TIMESTAMP) + {M.MARK}
          AND e.period_end >= j.session_date - INTERVAL 400 DAY
        GROUP BY 1, 2
    ),
    base AS (
        SELECT j.t, j.cik, j.pline, j.nyse, j.fme AS me_jun, d.me_dec, coalesce(bf.be, ba.be) AS be,
               bf.be IS NOT NULL AS be_fy
        FROM june j JOIN dec d USING (cik, t)
        LEFT JOIN be_fy bf USING (cik, t) LEFT JOIN be_any ba USING (cik, t)
        WHERE coalesce(bf.be, ba.be) > 0 AND d.me_dec > 0
    ),
    bp AS (
        SELECT t, quantile_cont(me_jun, 0.5) FILTER (WHERE nyse) AS size_bp,
               quantile_cont(be / me_dec, 0.3) FILTER (WHERE nyse) AS bm30,
               quantile_cont(be / me_dec, 0.7) FILTER (WHERE nyse) AS bm70,
               count(*) FILTER (WHERE nyse) AS n_nyse, count(*) AS n_all
        FROM base GROUP BY 1
    ),
    port AS (
        SELECT b.t, b.cik, b.pline, b.be_fy,
               CASE WHEN b.me_jun <= bp.size_bp THEN 'S' ELSE 'B' END ||
               CASE WHEN b.be / b.me_dec <= bp.bm30 THEN 'L' WHEN b.be / b.me_dec <= bp.bm70 THEN 'M' ELSE 'H' END AS p
        FROM base b JOIN bp USING (t)
    ),
    lret AS (
        SELECT security_id, CAST(date_trunc('month', session_date) AS DATE) AS month, exp(sum(ln(1 + ret))) - 1 AS mret,
               bool_or(ret_guarded OR ret IS NULL) AS bad
        FROM u GROUP BY 1, 2
    ),
    held AS (
        SELECT p.p, lr.month, lr.mret, w.fme AS w
        FROM port p
        JOIN lret lr ON lr.security_id = p.pline
             AND lr.month >= make_date(p.t, 7, 1) AND lr.month < make_date(p.t + 1, 7, 1)
        JOIN fm w ON w.cik = p.cik AND w.month = CAST(lr.month - INTERVAL 1 MONTH AS DATE)
        WHERE NOT lr.bad AND lr.month <= DATE '{end}'
    )
    SELECT month, p, sum(w * mret) / sum(w) AS ret, count(*) AS n FROM held GROUP BY 1, 2 ORDER BY 1, 2
    """


def pearson(x: list[float], y: list[float]) -> float | None:
    n = len(x)
    if n < 3:
        return None
    mx, my = sum(x) / n, sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else None


def factor_checks(con, end: dt.date) -> dict[str, Any]:
    if end > HOLDOUT_END:
        raise ValueError(f"ruling D6: return statistics end on or before {HOLDOUT_END}")
    root = C.build_root()
    fr = root / "reference" / "french"
    rates = (root / "reference" / "rates_daily.parquet").as_posix()
    res: dict[str, Any] = {"rule": RULE, "window_end": end.isoformat(), "holdout_rule": "D6", "gates": GATES}
    # risk-free: DTB3 of the last observation before the period
    rf_m = dict(con.execute(f"""
        SELECT m, arg_max(value_pct, obs_date) / 1200.0 FROM (
            SELECT CAST(date_trunc('month', obs_date) + INTERVAL 1 MONTH AS DATE) AS m, obs_date, value_pct
            FROM read_parquet('{rates}') WHERE series_id = 'DTB3') GROUP BY 1""").fetchall())
    ff3m = {r[0]: r[1:] for r in con.execute(
        f"SELECT month, mkt_rf, smb, hml FROM read_parquet('{(fr / 'ff3_monthly.parquet').as_posix()}')").fetchall()}
    ff3d = {r[0]: r[1] for r in con.execute(
        f"SELECT date, mkt_rf FROM read_parquet('{(fr / 'ff3_daily.parquet').as_posix()}')").fetchall()}
    for universe in ("member_common", "all_common"):
        mon = con.execute(f"""SELECT CAST(date_trunc('month', period_end) AS DATE), vw FROM idx
                              WHERE freq = 'M' AND universe = '{universe}' AND period_end <= DATE '{end}'
                              ORDER BY 1""").fetchall()
        pts = [(m, vw - rf_m[m], ff3m[m][0]) for m, vw in mon if m in rf_m and m in ff3m and m >= dt.date(2018, 2, 1)]
        day = con.execute(f"""
            SELECT i.period_end, i.vw, (SELECT arg_max(value_pct, obs_date) FROM read_parquet('{rates}')
                                          WHERE series_id = 'DTB3' AND obs_date < i.period_end) AS dtb3,
                   date_diff('day', lag(i.period_end) OVER (ORDER BY i.period_end), i.period_end) AS gap
            FROM idx i WHERE freq = 'D' AND universe = '{universe}' AND period_end <= DATE '{end}'
            ORDER BY 1""").fetchall()
        dpts = [(d, vw - dtb3 / 36000.0 * gap, ff3d[d]) for d, vw, dtb3, gap in day
                if gap is not None and dtb3 is not None and d in ff3d]
        res[f"mkt_rf_{universe}"] = {
            "monthly_rho": pearson([p[1] for p in pts], [p[2] for p in pts]), "months": len(pts),
            "first_month": pts[0][0].isoformat() if pts else None,
            "monthly_mean_diff": (sum(p[1] - p[2] for p in pts) / len(pts)) if pts else None,
            "daily_rho": pearson([p[1] for p in dpts], [p[2] for p in dpts]), "days": len(dpts)}
    rows = con.execute(ff_sorts_sql(end)).fetchall()
    ports: dict[dt.date, dict[str, float]] = {}
    counts: dict[str, list[int]] = {}
    for month, p, ret, n in rows:
        ports.setdefault(month, {})[p] = ret
        counts.setdefault(p, []).append(n)
    smb, hml = {}, {}
    for m, pr in ports.items():
        if len(pr) == 6:
            smb[m] = (pr["SL"] + pr["SM"] + pr["SH"]) / 3 - (pr["BL"] + pr["BM"] + pr["BH"]) / 3
            hml[m] = (pr["SH"] + pr["BH"]) / 2 - (pr["SL"] + pr["BL"]) / 2
    ms = sorted(m for m in smb if m in ff3m)
    res["smb"] = {"monthly_rho": pearson([smb[m] for m in ms], [ff3m[m][1] for m in ms]), "months": len(ms),
                  "first_month": ms[0].isoformat() if ms else None}
    res["hml"] = {"monthly_rho": pearson([hml[m] for m in ms], [ff3m[m][2] for m in ms]), "months": len(ms)}
    res["portfolio_firms_mean"] = {p: round(sum(v) / len(v), 1) for p, v in sorted(counts.items())}
    mk = res["mkt_rf_member_common"]["monthly_rho"]
    res["pass"] = {"mkt_rf_monthly_rho": bool(mk is not None and mk >= GATES["mkt_rf_monthly_rho"]),
                   "smb_monthly_rho": bool(res["smb"]["monthly_rho"] is not None
                                           and res["smb"]["monthly_rho"] >= GATES["smb_monthly_rho"])}
    return res


def build(end: dt.date = HOLDOUT_END) -> dict[str, Any]:
    receipt: dict[str, Any] = {"rule": RULE}
    con = C.connect(memory="300MB", threads=2, db_file="mkt_index.duckdb")
    with C.timed(receipt, "universe"):
        load_universe(con, receipt)
    with C.timed(receipt, "index"):
        build_index(con, receipt)
    M.publish_part(OUT_DIR, STAGE, SCHEMA, MODULES, {"rule": RULE, "rule_text": __doc__, "receipt": receipt,
                                                    "staleness": "daily"},
                   M.input_manifests(*INPUTS), pattern="index_returns.parquet")
    with C.timed(receipt, "factor_checks"):
        checks = factor_checks(con, end)
    checks["input_manifests_sha256"] = M.input_manifests(*INPUTS)
    C.write_json_atomic(C.stage_dir("validation") / "factors_market.json", checks)
    receipt["factor_checks"] = checks
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build",), default="build")
    ap.add_argument("--end", type=dt.date.fromisoformat, default=HOLDOUT_END)
    args = ap.parse_args(argv)
    rec = build(args.end)
    print(json.dumps(rec, default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
