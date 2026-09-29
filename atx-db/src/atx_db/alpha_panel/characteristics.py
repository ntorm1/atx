"""Stage C: raw daily characteristics from the assembled panel.

Values are the scorecard's base quantities BEFORE ranking/decay, oriented with the literature
prior sign (higher = predicted long side), plus additional characteristics. Every window is
over the line's own sessions and is NULL unless it spans exactly the required number of
calendar sessions (the line traded on every session of the window). ``r`` is the panel ``ret``
with guarded returns treated as missing; ``m`` is ``mkt_ret``.

Cross-sectional group statistics (industry momentum, within-industry momentum, industry-adjusted
reversal) use the same session's ``member`` lines of the FF49 group with at least 3 members.

Output: ``characteristics/year=YYYY/characteristics.parquet`` keyed by (session_date, security_id).
"""

from __future__ import annotations

import argparse
import math
import sys
from typing import Any

from . import common as C
from .prices import BUCKETS

SQRT252 = math.sqrt(252.0)

# name -> (description, citation, prior-oriented SQL over the line-window CTE ``w``)
SCORECARD = {
    "chtax": "(txt_q - txt_q_lag4) / nullif(at_lag4, 0)",
    "droe": "CASE WHEN be_lag1q > 0 AND be_lag1q_lag4 > 0 THEN ni_q / be_lag1q - ni_q_lag4 / be_lag1q_lag4 END",
    "ear": "ear_raw",
    "sue": "sue",
    "asset_growth": 'CASE WHEN at_lag4 > 0 THEN -("at" / at_lag4 - 1) END',
    "noa_at": "CASE WHEN at_lag4 > 0 THEN -(noa / at_lag4) END",
    "issuance_xbrl": "CASE WHEN shrs_q > 0 AND shrs_q_lag4 > 0 THEN -ln(shrs_q / shrs_q_lag4) END",
    "issuance_vendor": "CASE WHEN adj_shares > 0 AND adj_shares_252 > 0 THEN -ln(adj_shares / adj_shares_252) END",
    "low_beta": "-beta_252",
    "low_ivol": "-ivol_21",
    "low_max": "-max_21",
    "lowvol_ind": "-vol_252",
    "iv_rv_spread": "iv21_bf - vol_21 * {sqrt252}",
    "high_52w": "close / nullif(max_close_252, 0)",
    "mom_12_1": "mom_12_1",
    "ind_mom_12_1": "ind_mom_12_1",
    "within_ind_mom": "within_ind_mom",
    "accruals": 'CASE WHEN "at" > 0 AND at_lag4 > 0 THEN -((ni_ttm - cfo_ttm) / (("at" + at_lag4) / 2)) END',
    "cfoa": 'CASE WHEN "at" > 0 AND at_lag4 > 0 THEN cfo_ttm / (("at" + at_lag4) / 2) END',
    "fscore": "fscore",
    "gpa": 'CASE WHEN "at" > 0 THEN gp_ttm / "at" END',
    "opbe": "CASE WHEN be > 0 THEN oi_ttm / be END",
    "opex_at": 'CASE WHEN "at" > 0 THEN (sale_ttm - oi_ttm) / "at" END',
    "roa": 'CASE WHEN "at" > 0 THEN ni_ttm / "at" END',
    "roe_q": "CASE WHEN be_lag1q > 0 THEN ni_q / be_lag1q END",
    "ind_adj_rev_5": "ind_adj_rev_5",
    "seasonality_same_month": "seas_same_month",
    "dtc": "-si_dtc",
    "si_change": "-(si_ratio_raw - si_ratio_21)",
    "si_ratio": "-si_ratio_raw",
    "bm": "CASE WHEN me_company > 0 AND be > 0 THEN be / me_company END",
    "cfp": "CASE WHEN me_company > 0 THEN cfo_ttm / me_company END",
    "ebit_ev": "CASE WHEN me_company + coalesce(debt, 0) - coalesce(che, 0) > 0 AND oi_ttm > 0 THEN oi_ttm / (me_company + coalesce(debt, 0) - coalesce(che, 0)) END",
    "ep": "CASE WHEN me_company > 0 AND ni_ttm > 0 THEN ni_ttm / me_company END",
    "fcfp": "CASE WHEN me_company > 0 THEN (cfo_ttm - capx_ttm) / me_company END",
    "net_payout": "CASE WHEN me_company > 0 THEN (dvc_ttm + prstkc_ttm - sstk_ttm) / me_company END",
    "rd_me": "CASE WHEN me_company > 0 AND xrd_ttm > 0 THEN xrd_ttm / me_company END",
    "sp": "CASE WHEN me_company > 0 AND sale_ttm > 0 THEN sale_ttm / me_company END",
}

# Additional characteristics (prior-oriented; citations in docs/ALPHA_PANEL.md).
EXTRA = {
    "resid_mom_12_1": "resid_mom_12_1",          # Blitz, Huij & Martens (2011) residual momentum
    "rev_21": "-ret_21",                          # Jegadeesh (1990) one-month reversal
    "ind_adj_rev_21": "ind_adj_rev_21",           # Da, Liu & Schaumburg (2014)
    "iv_term_slope": "iv_atm_252d - iv_atm_21d",  # Vasquez (2017) IV term structure slope (+)
    "iv_change_21": "-(iv_atm_63d - iv63_21)",    # rising ATM IV predicts lower returns (Ang et al. 2010 dIV puts)
    "iv_rv_ratio": "CASE WHEN vol_21 > 0 THEN iv21_bf / (vol_21 * {sqrt252}) END",
    "short_vol_ratio_5": "-svr_5",                # Diether, Lee & Werner (2009) short selling
    "short_vol_ratio_21": "-svr_21",
    "abn_turnover": "abn_turnover",               # Gervais, Kaniel & Mingelgrin (2001) volume premium (+)
    "amihud_21": "amihud_21",                     # Amihud (2002) illiquidity premium (+)
    "hl_spread_21": "hl_spread_21",               # Corwin & Schultz (2012) spread estimate (cost input; not signed)
    "beta_252": "beta_252",
    "vol_21": "vol_21",
    "vol_252": "vol_252",
    "ivol_21": "ivol_21",
    "turnover_21": "CASE WHEN shares_out > 0 THEN vol_avg_21 / shares_out END",
    "log_me": "CASE WHEN me_company > 0 THEN ln(me_company) END",
    "log_me_line": "CASE WHEN me_line > 0 THEN ln(me_line) END",
    "sale_growth_q": "CASE WHEN sale_q_lag4 > 0 THEN sale_q / sale_q_lag4 - 1 END",  # Lakonishok et al. (1994) (-) raw
    "gm_change": "gm_change",                     # Abarbanell & Bushee (1998) gross margin change (+)
    "leverage": 'CASE WHEN "at" > 0 THEN debt / "at" END',
    "cash_at": 'CASE WHEN "at" > 0 THEN che / "at" END',
    "capx_at": 'CASE WHEN "at" > 0 THEN -(capx_ttm / "at") END',  # Titman, Wei & Xie (2004) investment (-)
    "dvc_yield": "CASE WHEN me_company > 0 THEN dvc_ttm / me_company END",
    "earn_days_since": "earn_days_since",
    "si_to_adv": "CASE WHEN vol_avg_21 > 0 THEN -(si_shares / vol_avg_21) END",  # unfloored days-to-cover
}


def _w(k: int) -> str:
    return f"(PARTITION BY security_id ORDER BY session_date ROWS BETWEEN {k - 1} PRECEDING AND CURRENT ROW)"


def _full(k: int) -> str:
    """The k-row window ending at the row spans exactly k consecutive sessions."""
    return f"(sidx - lag(sidx, {k - 1}) OVER (PARTITION BY security_id ORDER BY session_date) = {k - 1})"


def _panel_glob() -> str:
    return (C.build_root() / "panel" / "*" / "*.parquet").as_posix()


def line_bucket(con, b: int) -> dict[str, Any]:
    dest = C.build_root() / "_tmp" / "char_line" / f"bucket_{b:02d}.parquet"
    dest.parent.mkdir(parents=True, exist_ok=True)
    cal = C.calendar_path().as_posix()
    sql = f"""
    WITH cal AS (SELECT session_date, row_number() OVER (ORDER BY session_date) AS sidx FROM read_parquet('{cal}')),
    p AS (
        SELECT p.*, cal.sidx,
               CASE WHEN ret_guarded THEN NULL ELSE ret END AS r,
               mkt_ret AS m,
               CASE WHEN close > 0 AND raw_close > 0 AND shares_out > 0 THEN shares_out * raw_close / close END AS adj_shares,
               CASE WHEN shares_out > 0 THEN si_shares / shares_out END AS si_ratio_raw,
               CASE WHEN sv_total_volume > 0 THEN sv_short_volume / sv_total_volume END AS svr_1,
               CASE WHEN high > 0 AND low > 0 AND high >= low THEN ln(high / low) END AS hl
        FROM read_parquet('{_panel_glob()}', hive_partitioning = false, union_by_name = true) p JOIN cal USING (session_date)
        WHERE p.security_id % {BUCKETS} = {b}
    ),
    a AS (
        SELECT *,
            {_full(3)} AS f3, {_full(5)} AS f5, {_full(21)} AS f21, {_full(63)} AS f63, {_full(250)} AS f250,
            {_full(252)} AS f252,
            sidx - lag(sidx, 21) OVER lw = 21 AS g21, sidx - lag(sidx, 252) OVER lw = 252 AS g252,
            sidx - lag(sidx, 224) OVER lw = 224 AS g224, sidx - lag(sidx, 245) OVER lw = 245 AS g245,
            sidx - lag(sidx, 5) OVER lw = 5 AS g5,
            lag(close, 5) OVER lw AS close_5, lag(close, 21) OVER lw AS close_21, lag(close, 252) OVER lw AS close_252,
            lag(close, 224) OVER lw AS close_224, lag(close, 245) OVER lw AS close_245,
            lag(adj_shares, 252) OVER lw AS adj_shares_252_raw,
            lag(si_ratio_raw, 21) OVER lw AS si_ratio_21_raw,
            lag(iv_atm_63d, 21) OVER lw AS iv63_21_raw,
            sum(r) OVER {_w(3)} AS r3, sum(m) OVER {_w(3)} AS m3,
            stddev_samp(r) OVER {_w(21)} AS sd_r21, count(r) OVER {_w(21)} AS n_r21,
            corr(r, m) OVER {_w(21)} AS c_rm21,
            max(r) OVER {_w(21)} AS max_r21,
            stddev_samp(r) OVER {_w(252)} AS sd_r252, count(r) OVER {_w(252)} AS n_r252,
            stddev_samp(m) OVER {_w(252)} AS sd_m252,
            max(close) OVER {_w(252)} AS max_close_252_raw,
            avg(volume) OVER {_w(21)} AS vol_avg_21_raw,
            avg(volume) OVER {_w(252)} AS vol_avg_252,
            avg(CASE WHEN dollar_volume > 0 AND r IS NOT NULL THEN abs(r) / dollar_volume * 1e6 END) OVER {_w(21)} AS amihud_raw,
            avg(svr_1) OVER {_w(5)} AS svr_5_raw, count(svr_1) OVER {_w(5)} AS n_svr5,
            avg(svr_1) OVER {_w(21)} AS svr_21_raw, count(svr_1) OVER {_w(21)} AS n_svr21,
            last_value(iv_atm_21d IGNORE NULLS) OVER {_w(6)} AS iv21_bf,
            max(CASE WHEN earn_flag = '0' THEN sidx END) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS UNBOUNDED PRECEDING) AS last_earn_sidx,
            lag(earn_recent) OVER lw AS earn_recent_prev,
            -- Corwin-Schultz two-day inputs
            hl * hl + lag(hl) OVER lw * lag(hl) OVER lw AS cs_beta,
            CASE WHEN greatest(high, lag(high) OVER lw) > 0 AND least(low, lag(low) OVER lw) > 0
                 THEN ln(greatest(high, lag(high) OVER lw) / least(low, lag(low) OVER lw)) END AS cs_gamma_ln
        FROM p
        WINDOW lw AS (PARTITION BY security_id ORDER BY session_date)
    ),
    b1 AS (
        SELECT *,
            CASE WHEN f250 THEN corr(r3, m3) OVER {_w(250)} END AS c_r3m3_250,
            CASE WHEN g21 AND g252 AND close_252 > 0 THEN close_21 / close_252 - 1 END AS mom_12_1,
            CASE WHEN g5 AND close_5 > 0 THEN close / close_5 - 1 END AS ret_5,
            CASE WHEN g21 AND close_21 > 0 THEN close / close_21 - 1 END AS ret_21,
            CASE WHEN g224 AND g245 AND close_245 > 0 THEN close_224 / close_245 - 1 END AS seas_same_month,
            CASE WHEN g252 THEN adj_shares_252_raw END AS adj_shares_252,
            CASE WHEN g21 THEN si_ratio_21_raw END AS si_ratio_21,
            CASE WHEN g21 THEN iv63_21_raw END AS iv63_21,
            CASE WHEN f21 AND n_r21 >= 21 THEN sd_r21 END AS vol_21,
            CASE WHEN f21 AND n_r21 >= 21 THEN sd_r21 * sqrt(greatest(1 - c_rm21 * c_rm21, 0)) END AS ivol_21,
            CASE WHEN f21 AND n_r21 >= 21 THEN max_r21 END AS max_21,
            CASE WHEN f252 AND n_r252 >= 240 THEN sd_r252 END AS vol_252,
            CASE WHEN f252 THEN max_close_252_raw END AS max_close_252,
            CASE WHEN f21 THEN vol_avg_21_raw END AS vol_avg_21,
            CASE WHEN f21 AND f252 AND vol_avg_252 > 0 THEN vol_avg_21_raw / vol_avg_252 - 1 END AS abn_turnover,
            CASE WHEN f21 THEN amihud_raw END AS amihud_21,
            CASE WHEN n_svr5 >= 4 THEN svr_5_raw END AS svr_5,
            CASE WHEN n_svr21 >= 15 THEN svr_21_raw END AS svr_21,
            CASE WHEN last_earn_sidx IS NOT NULL THEN sidx - last_earn_sidx END AS earn_days_since,
            -- EAR: 3-session abnormal return ending on the session after the reaction day (earn_recent now and before)
            CASE WHEN earn_recent = 1 AND earn_recent_prev = 1 AND f3 THEN r3 - m3 END AS ear_point,
            CASE WHEN cs_beta IS NOT NULL AND cs_gamma_ln IS NOT NULL THEN
                 (sqrt(2 * cs_beta) - sqrt(cs_beta)) / (3 - 2 * sqrt(2)) - sqrt(cs_gamma_ln * cs_gamma_ln / (3 - 2 * sqrt(2))) END AS cs_alpha
        FROM a
    ),
    b2 AS (
        SELECT *,
            CASE WHEN f252 AND c_r3m3_250 IS NOT NULL AND sd_m252 > 0 AND vol_252 IS NOT NULL
                 THEN c_r3m3_250 * vol_252 / sd_m252 END AS beta_252,
            last_value(ear_point IGNORE NULLS) OVER (PARTITION BY security_id ORDER BY session_date
                ROWS BETWEEN 125 PRECEDING AND CURRENT ROW) AS ear_raw,
            avg(CASE WHEN cs_alpha IS NOT NULL THEN greatest(2 * (exp(cs_alpha) - 1) / (1 + exp(cs_alpha)), 0) END)
                OVER {_w(21)} AS hl_spread_raw
        FROM b1
    ),
    b3 AS (
        SELECT *,
            r - coalesce(beta_252, 1) * m AS resid_r,
            CASE WHEN f21 THEN hl_spread_raw END AS hl_spread_21
        FROM b2
    ),
    b4 AS (
        SELECT *,
            -- residual momentum: sum of residual returns t-251..t-21 scaled by their stdev
            sum(resid_r) OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN 251 PRECEDING AND 21 PRECEDING) AS rs_sum,
            stddev_samp(resid_r) OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN 251 PRECEDING AND 21 PRECEDING) AS rs_sd,
            count(resid_r) OVER (PARTITION BY security_id ORDER BY session_date ROWS BETWEEN 251 PRECEDING AND 21 PRECEDING) AS rs_n
        FROM b3
    ),
    b5 AS (
        SELECT *,
            CASE WHEN f252 AND rs_n >= 200 AND rs_sd > 0 THEN rs_sum / rs_sd END AS resid_mom_12_1,
            CASE WHEN sale_ttm > 0 AND gp_ttm IS NOT NULL THEN gp_ttm / sale_ttm END AS gm_now
        FROM b4
    )
    SELECT session_date, security_id, member, grp_ff49, grp_ff12, *
        EXCLUDE (session_date, security_id, member, grp_ff49, grp_ff12)
    FROM b5
    """
    # keep only the columns later stages need
    keep = [
        "session_date", "security_id", "member", "grp_ff49", "grp_ff12", "close", "iv_atm_21d", "iv_atm_63d",
        "iv_atm_252d", "si_dtc", "si_shares", "shares_out", "me_company", "me_line", "sue", "fscore",
        "txt_q", "txt_q_lag4", "at", "at_lag4", "ni_q", "be_lag1q", "ni_q_lag4", "be_lag1q_lag4", "noa", "shrs_q",
        "shrs_q_lag4", "ni_ttm", "cfo_ttm", "gp_ttm", "oi_ttm", "be", "sale_ttm", "debt", "che", "capx_ttm", "dvc_ttm",
        "prstkc_ttm", "sstk_ttm", "xrd_ttm", "sale_q", "sale_q_lag4", "adj_shares", "adj_shares_252", "si_ratio_raw",
        "si_ratio_21", "iv63_21", "iv21_bf", "vol_21", "ivol_21", "max_21", "vol_252", "max_close_252", "vol_avg_21",
        "abn_turnover", "amihud_21", "svr_5", "svr_21", "earn_days_since", "ear_raw", "beta_252", "hl_spread_21",
        "resid_mom_12_1", "mom_12_1", "ret_5", "ret_21", "seas_same_month", "gm_now",
    ]
    rows = C.copy_to_parquet(con, "SELECT " + ", ".join(f'"{k}"' for k in keep) + f" FROM ({sql})", dest)
    return {"rows": rows}


def year_pass(con, year: int) -> dict[str, Any]:
    src = (C.build_root() / "_tmp" / "char_line").as_posix() + "/*.parquet"
    dest = C.stage_dir("characteristics") / f"year={year}" / "characteristics.parquet"
    # gross margin change needs the year-ago TTM margin; use the value 252 sessions back on the same line
    sc = {k: v.format(sqrt252=SQRT252) for k, v in SCORECARD.items()}
    ex = {k: v.format(sqrt252=SQRT252) for k, v in EXTRA.items()}
    exprs = [f"{v} AS {k}" for k, v in {**sc, **ex}.items()]
    sql = f"""
    WITH l AS (
        SELECT *, lag(gm_now, 252) OVER (PARTITION BY security_id ORDER BY session_date) AS gm_252
        FROM read_parquet('{src}')
        WHERE session_date BETWEEN DATE '{year - 2}-01-01' AND DATE '{year}-12-31'
    ),
    y AS (SELECT *, gm_now - gm_252 AS gm_change FROM l WHERE session_date BETWEEN DATE '{year}-01-01' AND DATE '{year}-12-31'),
    g AS (
        SELECT session_date, grp_ff49,
               avg(mom_12_1) AS g_mom, avg(ret_5) AS g_r5, avg(ret_21) AS g_r21,
               count(mom_12_1) AS n_mom, count(ret_5) AS n_r5, count(ret_21) AS n_r21
        FROM y WHERE member AND grp_ff49 IS NOT NULL GROUP BY 1, 2
    ),
    z AS (
        SELECT y.*,
            CASE WHEN g.n_mom >= 3 THEN g.g_mom END AS ind_mom_12_1,
            CASE WHEN g.n_mom >= 3 THEN y.mom_12_1 - g.g_mom END AS within_ind_mom,
            CASE WHEN g.n_r5 >= 3 THEN -(y.ret_5 - g.g_r5) END AS ind_adj_rev_5,
            CASE WHEN g.n_r21 >= 3 THEN -(y.ret_21 - g.g_r21) END AS ind_adj_rev_21
        FROM y LEFT JOIN g USING (session_date, grp_ff49)
    )
    SELECT session_date, security_id, member, {', '.join(exprs)}
    FROM z ORDER BY session_date, security_id
    """
    rows = C.copy_to_parquet(con, sql, dest)
    return {"rows": rows}


def run(stages: list[str], years: list[int]) -> dict[str, Any]:
    receipt: dict[str, Any] = {"stage": "characteristics", "scorecard": SCORECARD, "extra": EXTRA}
    con = C.connect(memory="650MB", threads=2)
    if "line" in stages:
        with C.timed(receipt, "line"):
            receipt["line"] = {}
            for b in range(BUCKETS):
                receipt["line"][str(b)] = line_bucket(con, b)
                print("bucket", b, receipt["line"][str(b)], flush=True)
    if "year" in stages:
        with C.timed(receipt, "year"):
            receipt["year"] = {str(y): year_pass(con, y) for y in years}
    C.write_json_atomic(C.stage_dir("characteristics") / "manifest.json", receipt)
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stages", default="line,year")
    ap.add_argument("--years", default="2019-2026")
    args = ap.parse_args(argv)
    a, _, b = args.years.partition("-")
    run([s.strip() for s in args.stages.split(",") if s.strip()], list(range(int(a), int(b or a) + 1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
